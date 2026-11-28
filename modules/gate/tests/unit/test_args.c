/*
 * Gate data type: unit tests of the command arguments (gate_args.c).
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * WHERE THE EXPECTATIONS COME FROM. The grammar, its limits and its
 * defaults are those of SPEC.md section 2 and of gate_args.h. A parsed
 * number is compared with the C literal of the same decimal, which the
 * compiler rounds without help from the parser. IEEE-754 bit patterns were
 * computed with Python's struct module. A message the parser words itself
 * is compared in full; one that comes from gateConfigValidate() only by the
 * option it names, since its wording belongs to the core.
 *
 * NO TERMINATORS. An argument is a (pointer, length) pair. Every argument
 * given to a parser here lives in a heap block of exactly its length, and
 * so does the vector of a command, so AddressSanitizer and valgrind catch a
 * parser that reads one byte, or one argument, too far.
 */

#include <float.h>
#include <stdarg.h>
#include "testutil.h"
#include "gate_args.h"

#define COUNTOF(a) (sizeof(a) / sizeof((a)[0]))

/* A string literal and its length, NULs inside it included. */
#define LIT(s) (s), (sizeof(s) - 1)

typedef struct bytes {
    const char *s;
    size_t len;
} bytes;

/* CHECK for table driven tests: a failure also shows the row. */
#define CHECK_ON(input, cond) do { \
    int failed_ = gate_test_failures; \
    CHECK(cond); \
    if (gate_test_failures != failed_) printf("    with \"%s\"\n", (input)); \
} while (0)

/* 'n' copies of 'c' as a C string in 'buf'. */
static const char *repeat(char *buf, char c, size_t n) {
    memset(buf, c, n);
    buf[n] = '\0';
    return buf;
}

/* ----------------------------- Exact arguments --------------------------- */

static char *exact(const char *s, size_t len) {
    char *p = malloc(len ? len : 1);

    if (p == NULL) abort();
    memcpy(p, s, len);
    return p;
}

#define EXACT_PARSER(name, parser, type) \
    static int name(const char *s, size_t len, type *out) { \
        char *p = exact(s, len); \
        int ok = parser(p, len, out); \
        free(p); \
        return ok; \
    }

EXACT_PARSER(parse_double, gateParseDouble, double)
EXACT_PARSER(parse_u64, gateParseU64, uint64_t)
EXACT_PARSER(parse_hex64, gateParseHex64, uint64_t)
EXACT_PARSER(parse_bits, gateParseDoubleBits, double)

static int parse_id(const char *s, size_t len, uint64_t *epoch, uint64_t *seq) {
    char *p = exact(s, len);
    int ok = gateParseId(p, len, epoch, seq);

    free(p);
    return ok;
}

static int arg_is(const char *s, size_t len, const char *word) {
    gateArg a = {exact(s, len), len};
    int ok = gateArgIs(&a, word);

    free((void *)a.ptr);
    return ok;
}

#define MAXARGS 256

typedef struct args {
    gateArg v[MAXARGS];
    int n;
} args;

static void args_add(args *a, const char *s, size_t len) {
    if (a->n == MAXARGS) abort();
    a->v[a->n].ptr = exact(s, len);
    a->v[a->n].len = len;
    a->n++;
}

/* Append C strings up to a NULL. */
static void args_addv(args *a, va_list ap) {
    const char *s;

    while ((s = va_arg(ap, const char *)) != NULL) args_add(a, s, strlen(s));
}

static void args_set(args *a, ...) __attribute__((sentinel));
static void args_set(args *a, ...) {
    va_list ap;

    a->n = 0;
    va_start(ap, a);
    args_addv(a, ap);
    va_end(ap);
}

static void args_free(args *a) {
    while (a->n > 0) free((void *)a->v[--a->n].ptr);
}

/* The vector as a heap block of exactly its 'n' entries. */
static gateArg *args_exact(const args *a) {
    size_t size = (size_t)a->n * sizeof(a->v[0]);
    gateArg *v = malloc(size ? size : 1);

    if (v == NULL) abort();
    memcpy(v, a->v, size);
    return v;
}

/* ------------------------------ gateParseDouble -------------------------- */

TEST(double_accepts_decimal_numbers) {
    static const struct { const char *s; double v; } t[] = {
        {"0.05", 0.05}, {".5", 0.5}, {"5.", 5.0}, {"1e-3", 0.001}, {"-2", -2.0},
        {"+1", 1.0}, {"0", 0.0}, {"1E5", 100000.0}, {"+.5e+1", 5.0}, {"-12.50", -12.5},
        {"007", 7.0}, {"0.1", 0.1}, {"1e308", 1e308}, {"-1e-300", -1e-300},
        /* How Python, and so redis-py, writes small and large floats. */
        {"1e-05", 0.00001}, {"1e+300", 1e300},
        /* All the digits a double can need. */
        {"0.30000000000000004", 0.30000000000000004},
        /* The largest double, and a decimal above it that still rounds to it. */
        {"1.7976931348623157e308", DBL_MAX}, {"1.7976931348623158e308", DBL_MAX},
        /* Too small for a double is not an error: it rounds, like any
         * decimal, here to the smallest denormal and to zero. */
        {"4.9e-324", 0x1p-1074}, {"1e-999", 0.0}, {"0e999", 0.0}
    };
    double v;
    size_t i;

    for (i = 0; i < COUNTOF(t); i++) {
        v = 99.0;
        CHECK_ON(t[i].s, parse_double(t[i].s, strlen(t[i].s), &v) && v == t[i].v);
    }
    v = 99.0;
    CHECK(parse_double(LIT("-0"), &v) && v == 0.0 && signbit(v));
}

TEST(double_rejects_everything_else) {
    static const bytes t[] = {
        {LIT("")}, {LIT(" ")}, {LIT(" 1")}, {LIT("1 ")}, {LIT("\t1")}, {LIT("1\n")},
        {LIT("0x10")}, {LIT("0X10")}, {LIT("0x1p-3")}, {LIT("1p3")},
        {LIT("nan")}, {LIT("NaN")}, {LIT("NAN")}, {LIT("-nan")}, {LIT("nan(1)")},
        {LIT("inf")}, {LIT("Inf")}, {LIT("INF")}, {LIT("-inf")}, {LIT("+inf")},
        {LIT("infinity")}, {LIT("Infinity")}, {LIT("INFINITY")}, {LIT("-infinity")},
        /* Overflow, the last one by the smallest margin 17 digits can state. */
        {LIT("1e999")}, {LIT("-1e999")}, {LIT("1.7976931348623159e308")},
        {LIT("1e")}, {LIT("1e+")}, {LIT("e1")}, {LIT("E")}, {LIT(".")}, {LIT("+")}, {LIT("-")},
        {LIT("--1")}, {LIT("+-1")}, {LIT("1-")}, {LIT("1+1")}, {LIT("1.2.3")},
        {LIT("1e2e3")}, {LIT("1e2.5")}, {LIT("1,5")}, {LIT("1_000")}, {LIT("1f")},
        {LIT("1d")}, {LIT("1L")}, {LIT("abc")}, {LIT("0.05x")},
        /* A NUL is a byte like any other, wherever it is. */
        {LIT("1\0")}, {LIT("1\0junk")}, {LIT("\0")}, {LIT("\0" "1")}, {LIT("0.5\0")},
        {LIT("\xef\xbc\x91")} /* U+FF11, a full-width 1 */
    };
    size_t i;

    for (i = 0; i < COUNTOF(t); i++) {
        double v = 99.0;

        CHECK_ON(t[i].s, !parse_double(t[i].s, t[i].len, &v) && v == 99.0);
    }
}

TEST(double_length_limit) {
    char s[64];
    double v = 99.0;

    /* 62 zeros and a 1 are 63 bytes, the longest number accepted. */
    memset(s, '0', sizeof(s));
    s[62] = '1';
    CHECK(parse_double(s, 63, &v) && v == 1.0);
    /* One more zero makes it 10 in 64 bytes: refused for its length. */
    v = 99.0;
    CHECK(!parse_double(s, 64, &v) && v == 99.0);
    /* 63 bytes that all matter. */
    memset(s, '0', sizeof(s));
    s[0] = '1';
    CHECK(parse_double(s, 63, &v) && v == 1e62);
}

/* ------------------------------- gateParseU64 ---------------------------- */

TEST(u64_accepts_digits) {
    static const struct { const char *s; uint64_t v; } t[] = {
        {"0", 0}, {"1", 1}, {"42", 42}, {"007", 7},
        {"9223372036854775808", 9223372036854775808ull},   /* 2^63 */
        {"10000000000000000000", 10000000000000000000ull}, /* 20 digits */
        {"00000000000000000001", 1},                       /* 20 digits too */
        {"18446744073709551614", 18446744073709551614ull},
        {"18446744073709551615", UINT64_MAX}               /* 2^64 - 1 */
    };
    size_t i;

    for (i = 0; i < COUNTOF(t); i++) {
        uint64_t v = 99;

        CHECK_ON(t[i].s, parse_u64(t[i].s, strlen(t[i].s), &v) && v == t[i].v);
    }
}

TEST(u64_rejects_everything_else) {
    static const bytes t[] = {
        {LIT("")},
        {LIT("18446744073709551616")},  /* 2^64: the last digit overflows */
        {LIT("18446744073709551620")},  /* the multiplication overflows */
        {LIT("99999999999999999999")},
        {LIT("000000000000000000001")}, /* 21 digits, although it is 1 */
        {LIT("184467440737095516150")},
        {LIT("+1")}, {LIT("-1")}, {LIT("-0")}, {LIT(" 1")}, {LIT("1 ")}, {LIT("1.0")},
        {LIT("1e3")}, {LIT("0x10")}, {LIT("1a")}, {LIT("a")},
        {LIT("/")}, {LIT(":")},         /* the bytes next to '0' and '9' */
        {LIT("1\0")}, {LIT("\0")}, {LIT("1\0" "2")},
        {LIT("\xef\xbc\x91")}
    };
    size_t i;

    for (i = 0; i < COUNTOF(t); i++) {
        uint64_t v = 99;

        CHECK_ON(t[i].s, !parse_u64(t[i].s, t[i].len, &v) && v == 99);
    }
}

/* --------------------------------- gateArgIs ----------------------------- */

TEST(arg_is_case_insensitive_and_exact) {
    CHECK(arg_is(LIT("ALPHA"), "ALPHA"));
    CHECK(arg_is(LIT("alpha"), "ALPHA"));
    CHECK(arg_is(LIT("aLpHa"), "ALPHA"));
    CHECK(arg_is(LIT("alwaysdefer"), "ALWAYSDEFER"));
    CHECK(!arg_is(LIT("ALPH"), "ALPHA"));
    CHECK(!arg_is(LIT("ALPHAS"), "ALPHA"));
    CHECK(!arg_is(LIT("BETA"), "ALPHA"));
    CHECK(!arg_is(LIT("ALPHB"), "ALPHA"));
    CHECK(!arg_is(LIT(""), "ALPHA"));
    CHECK(!arg_is(LIT(" ALPHA"), "ALPHA"));
    CHECK(!arg_is(LIT("ALPHA "), "ALPHA"));
    CHECK(!arg_is(LIT("\xc1LPHA"), "ALPHA")); /* 'A' with the high bit set */
    /* A NUL does not end the argument. */
    CHECK(!arg_is(LIT("ALPHA\0"), "ALPHA"));
    CHECK(!arg_is(LIT("ALPH\0"), "ALPHA"));
    CHECK(!arg_is(LIT("AL\0HA"), "ALPHA"));
}

/* ---------------------------------- Hex64 -------------------------------- */

TEST(hex64_known_values) {
    static const struct { uint64_t v; const char *hex; } t[] = {
        {0, "0000000000000000"},
        {1, "0000000000000001"},
        {0x0123456789abcdefull, "0123456789abcdef"},
        {0x8000000000000000ull, "8000000000000000"},
        {UINT64_MAX, "ffffffffffffffff"}
    };
    size_t i;

    for (i = 0; i < COUNTOF(t); i++) {
        char *buf = malloc(17); /* all the formatter may write: 16 digits, a NUL */
        uint64_t v = 99;

        memset(buf, 'x', 17);
        gateFormatHex64(buf, t[i].v);
        CHECK_ON(t[i].hex, memcmp(buf, t[i].hex, 17) == 0);
        CHECK_ON(t[i].hex, parse_hex64(t[i].hex, 16, &v) && v == t[i].v);
        free(buf);
    }
}

TEST(hex64_parse_is_strict) {
    static const bytes bad[] = {
        {LIT("")},
        {LIT("0123456789abcde")},   /* 15 digits */
        {LIT("0123456789abcdef0")}, /* 17 digits */
        {LIT("0x23456789abcdef")}, {LIT("+123456789abcdef")}, {LIT("-123456789abcdef")},
        {LIT(" 123456789abcdef")}, {LIT("0123456789abcde ")},
        {LIT("0123456789abcde\0")}, {LIT("01234567\0" "9abcdef")},
        /* The bytes next to '0', '9', 'a', 'f', 'A' and 'F'. */
        {LIT("000000000000000/")}, {LIT("000000000000000:")}, {LIT("000000000000000`")},
        {LIT("000000000000000g")}, {LIT("000000000000000@")}, {LIT("000000000000000G")}
    };
    uint64_t v = 99;
    size_t i;

    /* Upper case is read as well (it is never written). */
    CHECK(parse_hex64(LIT("0123456789ABCDEF"), &v) && v == 0x0123456789abcdefull);
    CHECK(parse_hex64(LIT("FfFfFfFfFfFfFfFf"), &v) && v == UINT64_MAX);
    for (i = 0; i < COUNTOF(bad); i++) {
        v = 99;
        CHECK_ON(bad[i].s, !parse_hex64(bad[i].s, bad[i].len, &v) && v == 99);
    }
}

TEST(hex64_round_trip) {
    uint64_t rng = 64;
    char buf[17];
    int i, bad = 0;

    for (i = 0; i < 2000; i++) {
        uint64_t v = gate_test_rand64(&rng), back = 0;

        v >>= gate_test_rand64(&rng) % 64; /* short values, leading zeros */
        gateFormatHex64(buf, v);
        if (strlen(buf) != 16 || strspn(buf, "0123456789abcdef") != 16 ||
            strtoull(buf, NULL, 16) != v || !parse_hex64(buf, 16, &back) || back != v)
            bad++;
    }
    CHECK_EQ_INT(bad, 0);
}

/* ------------------------------- Double bits ----------------------------- */

TEST(double_bits_known_values) {
    static const struct { double v; const char *hex; } t[] = {
        {0.0, "0000000000000000"},
        {-0.0, "8000000000000000"},
        {0x1p-1074, "0000000000000001"}, /* the smallest denormal */
        {DBL_MIN, "0010000000000000"},   /* the smallest normal */
        {1.0 / 3.0, "3fd5555555555555"},
        {0.05, "3fa999999999999a"},
        {1.0, "3ff0000000000000"},
        {-2.5, "c004000000000000"},
        {DBL_MAX, "7fefffffffffffff"},
        {-DBL_MAX, "ffefffffffffffff"}
    };
    size_t i;

    for (i = 0; i < COUNTOF(t); i++) {
        char *buf = malloc(17);
        double v = 99.0;

        memset(buf, 'x', 17);
        gateFormatDoubleBits(buf, t[i].v);
        CHECK_ON(t[i].hex, memcmp(buf, t[i].hex, 17) == 0);
        /* Bit for bit: memcmp tells -0 from 0, which == does not. */
        CHECK_ON(t[i].hex, parse_bits(t[i].hex, 16, &v) && memcmp(&v, &t[i].v, sizeof(v)) == 0);
        free(buf);
    }
}

TEST(double_bits_parse_is_strict) {
    static const bytes bad[] = {
        {LIT("")},
        {LIT("3fd555555555555")},   /* 15 digits */
        {LIT("3fd55555555555555")}, /* 17 digits */
        {LIT("3fd555555555555g")}, {LIT("0x3fd55555555555")}, {LIT("3fd5555555555 55")},
        {LIT("-fd5555555555555")}, {LIT("3fd555555555555\0")},
        {LIT("0.33333333333333")}   /* 16 bytes, but a decimal */
    };
    double v = 99.0;
    size_t i;

    CHECK(parse_bits(LIT("3FD5555555555555"), &v) && v == 1.0 / 3.0);
    for (i = 0; i < COUNTOF(bad); i++) {
        v = 99.0;
        CHECK_ON(bad[i].s, !parse_bits(bad[i].s, bad[i].len, &v) && v == 99.0);
    }
}

/* "No nan / inf" holds for bits as it does for decimals: the patterns just
 * above the largest doubles of the table above are not numbers to parse. */
TEST(double_bits_must_be_finite) {
    static const char *const bad[] = {
        "7ff0000000000000", "fff0000000000000", /* +inf, -inf */
        "7ff8000000000000", "fff8000000000000", /* quiet NaNs */
        "7ff0000000000001", "7fffffffffffffff", /* the first and the last NaN */
        "ffffffffffffffff"
    };
    size_t i;

    for (i = 0; i < COUNTOF(bad); i++) {
        double v = 99.0;

        CHECK_ON(bad[i], !parse_bits(bad[i], 16, &v) && v == 99.0);
    }
}

TEST(double_bits_round_trip) {
    uint64_t rng = 53;
    char buf[17], again[17];
    int i, bad = 0;

    for (i = 0; i < 2000; i++) {
        uint64_t pattern = gate_test_rand64(&rng);
        double v, back = 99.0;

        /* An exponent of all ones is an infinity or a NaN: about one
         * pattern in two thousand. Those have their own test. */
        if (((pattern >> 52) & 0x7ff) == 0x7ff) continue;
        memcpy(&v, &pattern, sizeof(v));
        gateFormatDoubleBits(buf, v);
        if (strtoull(buf, NULL, 16) != pattern || !parse_bits(buf, 16, &back) ||
            memcmp(&back, &v, sizeof(v)) != 0)
            bad++;
        gateFormatDoubleBits(again, back);
        if (strcmp(buf, again) != 0) bad++;
    }
    CHECK_EQ_INT(bad, 0);
}

/* ----------------------------------- Ids --------------------------------- */

TEST(id_format) {
    char buf[GATE_ID_MAX], *tight;

    /* The example of gate_args.h. */
    CHECK_EQ_INT(gateFormatId(buf, sizeof(buf), 0x5d0c3f9a7b21e684ull, 1042), 21);
    CHECK_EQ_STR(buf, "5d0c3f9a7b21e684-1042");
    CHECK_EQ_INT(gateFormatId(buf, sizeof(buf), 0, 1), 18);
    CHECK_EQ_STR(buf, "0000000000000000-1");
    /* The longest id there is fits GATE_ID_MAX. */
    CHECK_EQ_INT(gateFormatId(buf, sizeof(buf), UINT64_MAX, UINT64_MAX), 37);
    CHECK_EQ_STR(buf, "ffffffffffffffff-18446744073709551615");

    /* A buffer of exactly the id and its terminator is enough; one byte
     * less gets nothing at all rather than an id cut short. */
    tight = malloc(38);
    CHECK_EQ_INT(gateFormatId(tight, 38, UINT64_MAX, UINT64_MAX), 37);
    CHECK(memcmp(tight, "ffffffffffffffff-18446744073709551615", 38) == 0);
    free(tight);
    tight = malloc(37);
    memset(tight, 'x', 37);
    CHECK_EQ_INT(gateFormatId(tight, 37, UINT64_MAX, UINT64_MAX), 0);
    CHECK(tight[0] == '\0');
    free(tight);
}

TEST(id_parse) {
    uint64_t epoch = 7, seq = 7;
    char buf[GATE_ID_MAX];

    CHECK(parse_id(LIT("5d0c3f9a7b21e684-1042"), &epoch, &seq));
    CHECK(epoch == 0x5d0c3f9a7b21e684ull && seq == 1042);
    CHECK(parse_id(LIT("0000000000000000-1"), &epoch, &seq) && epoch == 0 && seq == 1);
    CHECK(parse_id(LIT("ffffffffffffffff-18446744073709551615"), &epoch, &seq));
    CHECK(epoch == UINT64_MAX && seq == UINT64_MAX);
    CHECK(parse_id(LIT("0000000000000001-10000000000000000000"), &epoch, &seq));
    CHECK(epoch == 1 && seq == 10000000000000000000ull);

    /* Upper-case hex is read, and comes back in lower case. */
    CHECK(parse_id(LIT("5D0C3F9A7B21E684-1042"), &epoch, &seq));
    CHECK(epoch == 0x5d0c3f9a7b21e684ull && seq == 1042);
    gateFormatId(buf, sizeof(buf), epoch, seq);
    CHECK_EQ_STR(buf, "5d0c3f9a7b21e684-1042");
}

TEST(id_parse_is_strict) {
    static const bytes t[] = {
        {LIT("")},
        {LIT("5d0c3f9a7b21e68-1042")},   /* 15 hex digits */
        {LIT("5d0c3f9a7b21e6840-1042")}, /* 17 hex digits */
        {LIT("5d0c3f9a7b21e68g-1042")},
        {LIT("0x0c3f9a7b21e684-1042")},
        {LIT(" 5d0c3f9a7b21e684-1042")},
        {LIT("5d0c3f9a7b21e684-01042")}, /* leading zero */
        {LIT("5d0c3f9a7b21e684-00000000000000000001")},
        {LIT("5d0c3f9a7b21e684-0")},     /* seq 0 */
        {LIT("5d0c3f9a7b21e684-00")},
        {LIT("5d0c3f9a7b21e684-18446744073709551616")},  /* 2^64 */
        {LIT("5d0c3f9a7b21e684-184467440737095516150")}, /* 21 digits */
        {LIT("5d0c3f9a7b21e6841042")},   /* no dash */
        {LIT("5d0c3f9a7b21e684_1042")},
        {LIT("5d0c3f9a7b21e684 1042")},
        {LIT("5d0c3f9a7b21e684--1042")},
        {LIT("5d0c3f9a7b21e684-+1042")},
        {LIT("5d0c3f9a7b21e684-")},      /* no seq */
        {LIT("5d0c3f9a7b21e684")},
        {LIT("-1042")},
        {LIT("5d0c3f9a7b21e684-1042 ")}, /* trailing bytes */
        {LIT("5d0c3f9a7b21e684-1042\0")},
        {LIT("5d0c3f9a7b21e684-1042\n")},
        {LIT("5d0c3f9a7b21e684-1042x")},
        {LIT("5d0c3f9a7b21e684-1042-1")},
        {LIT("5d0c3f9a7b21e684-1e3")},
        {LIT("5d0c3f9a7b21e684-10.0")}
    };
    size_t i;

    for (i = 0; i < COUNTOF(t); i++) {
        uint64_t epoch = 7, seq = 7;

        /* Neither output is touched, not even by a good epoch. */
        CHECK_ON(t[i].s, !parse_id(t[i].s, t[i].len, &epoch, &seq) && epoch == 7 && seq == 7);
    }
}

TEST(id_round_trip) {
    uint64_t rng = 1042;
    char buf[GATE_ID_MAX];
    int i, bad = 0;

    for (i = 0; i < 2000; i++) {
        uint64_t epoch = gate_test_rand64(&rng), seq = gate_test_rand64(&rng), e = 0, q = 0;
        size_t n;

        seq >>= gate_test_rand64(&rng) % 64; /* ids of every length */
        if (seq == 0) seq = 1;
        n = gateFormatId(buf, sizeof(buf), epoch, seq);
        if (n != strlen(buf) || n < 18 || !parse_id(buf, n, &e, &q) || e != epoch || q != seq)
            bad++;
    }
    CHECK_EQ_INT(bad, 0);
}

/* --------------------------------- GCREATE ------------------------------- */

/* The server values every command below is parsed with. */
#define SNAPSHOT 250u
#define TIMEOUT 60000

static char g_err[256];

/* 'cfg' starts as garbage, the message empty: what a test then finds in
 * them is the parser's doing. */
static int create_args(gateConfig *cfg, const args *a) {
    gateArg *v = args_exact(a);
    int rc;

    memset(cfg, 0xff, sizeof(*cfg));
    g_err[0] = '\0';
    rc = gateParseCreate(v, a->n, SNAPSHOT, TIMEOUT, cfg, g_err, sizeof(g_err));
    free(v);
    return rc;
}

/* Parse a NULL-terminated list of C strings. The arguments are freed before
 * the caller looks at the config: one that still pointed into them would be
 * caught reading freed memory. */
static int create(gateConfig *cfg, ...) __attribute__((sentinel));
static int create(gateConfig *cfg, ...) {
    args a;
    va_list ap;
    int rc;

    a.n = 0;
    va_start(ap, cfg);
    args_addv(&a, ap);
    va_end(ap);
    rc = create_args(cfg, &a);
    args_free(&a);
    return rc;
}

static int zeroed(const gateConfig *c) {
    return c->kind == 0 && c->alpha == 0.0 && c->delta == 0.0 && c->audit == 0.0 &&
           c->nclasses == 0 && c->labels == NULL && c->class_alpha == NULL &&
           c->class_defer == NULL && c->snapshot_examples == 0 && c->feedback_timeout_ms == 0;
}

/* 1 if a parse was refused and left the config zeroed. */
static int refused(int rc, gateConfig *cfg) {
    if (rc == GATE_OK) gateConfigFree(cfg);
    return rc == GATE_ERR && zeroed(cfg);
}

/* The command must parse; the test ends here if it does not. */
#define CREATE_OK(cfg, ...) do { \
    int rc_ = create((cfg), __VA_ARGS__, NULL); \
    CHECK(rc_ == GATE_OK); \
    if (rc_ != GATE_OK) { \
        printf("    refused: %s\n", g_err); \
        return; \
    } \
} while (0)

/* The command is refused with exactly this message, the config zeroed. */
#define CREATE_FAILS(msg, ...) do { \
    gateConfig cfg_; \
    CHECK(refused(create(&cfg_, __VA_ARGS__, NULL), &cfg_)); \
    CHECK_EQ_STR(g_err, msg); \
} while (0)

/* The same for what gateConfigValidate() refuses: the message has to name
 * 'about', its wording is the core's. */
#define CREATE_INVALID(about, ...) do { \
    gateConfig cfg_; \
    CHECK(refused(create(&cfg_, __VA_ARGS__, NULL), &cfg_)); \
    CHECK(strstr(g_err, about) != NULL); \
} while (0)

/* Two valid commands for the tests to add to. */
#define SCORE_GATE "ALPHA", "0.1", "SCORE"
#define AB_GATE "ALPHA", "0.1", "CLASSES", "2", "a", "b"

/* What a command asks for. */
typedef struct want {
    int kind;
    double alpha, delta, audit;
    uint32_t n;
    const char *labels[4];
    double class_alpha[4];
    int defer[4];
} want;

/* The first field of a parsed config that is not what the command asked
 * for (with the server values above), or "" if every field is. */
static const char *differs(const gateConfig *c, const want *w) {
    uint32_t i;

    if (c->kind != w->kind) return "kind";
    if (c->alpha != w->alpha) return "alpha";
    if (c->delta != w->delta) return "delta";
    if (c->audit != w->audit) return "audit";
    if (c->snapshot_examples != SNAPSHOT) return "snapshot_examples";
    if (c->feedback_timeout_ms != TIMEOUT) return "feedback_timeout_ms";
    if (c->nclasses != w->n) return "nclasses";
    if (w->n == 0)
        return (c->labels || c->class_alpha || c->class_defer) ? "arrays of a score gate" : "";
    for (i = 0; i < w->n; i++) {
        if (strcmp(c->labels[i], w->labels[i]) != 0) return "labels";
        if (c->class_alpha[i] != w->class_alpha[i]) return "class_alpha";
        if (c->class_defer[i] != w->defer[i]) return "class_defer";
    }
    return "";
}

/* The two forms of GCREATE in SPEC.md section 2 with every option they
 * take, one option per row, and what they ask for. A label without a
 * CLASSALPHA of its own gets ALPHA. */
static const char *const classifier_options[6][8] = {
    {"ALPHA", "0.05"},
    {"CLASSES", "3", "billing", "bug", "other"},
    {"AUDIT", "0.1"},
    {"DELTA", "0.001"},
    {"ALWAYSDEFER", "1", "other"},
    {"CLASSALPHA", "2", "billing", "0.01", "bug", "0.02"}
};
static const want classifier_want = {
    .kind = GATE_KIND_CLASSIFIER, .alpha = 0.05, .delta = 0.001, .audit = 0.1, .n = 3,
    .labels = {"billing", "bug", "other"}, .class_alpha = {0.01, 0.02, 0.05}, .defer = {0, 0, 1}
};
static const char *const score_options[4][8] = {
    {"ALPHA", "0.02"}, {"SCORE"}, {"AUDIT", "0.2"}, {"DELTA", "0.05"}
};
static const want score_want = {
    .kind = GATE_KIND_SCORE, .alpha = 0.02, .delta = 0.05, .audit = 0.2
};

/* In the order SPEC.md writes them. */
TEST(create_classifier) {
    gateConfig cfg;

    CREATE_OK(&cfg, "ALPHA", "0.05", "CLASSES", "3", "billing", "bug", "other",
              "AUDIT", "0.1", "DELTA", "0.001", "ALWAYSDEFER", "1", "other",
              "CLASSALPHA", "2", "billing", "0.01", "bug", "0.02");
    CHECK_EQ_STR(differs(&cfg, &classifier_want), "");
    gateConfigFree(&cfg);
}

TEST(create_score) {
    gateConfig cfg;

    CREATE_OK(&cfg, "ALPHA", "0.02", "SCORE", "AUDIT", "0.2", "DELTA", "0.05");
    CHECK_EQ_STR(differs(&cfg, &score_want), "");
    gateConfigFree(&cfg);
}

/* Next permutation of p[0..n) in lexicographic order; 0 after the last. */
static int next_permutation(int *p, int n) {
    int i = n - 1, j = n - 1, t;

    while (i > 0 && p[i - 1] >= p[i]) i--;
    if (i == 0) return 0;
    while (p[j] <= p[i - 1]) j--;
    t = p[i - 1], p[i - 1] = p[j], p[j] = t;
    for (j = n - 1; i < j; i++, j--) t = p[i], p[i] = p[j], p[j] = t;
    return 1;
}

/* Parse the options 'opt' (rows of words, each ended by a NULL) in every
 * order. Returns how many orders there were; '*bad' counts those that did
 * not give 'w'. */
static int every_order(const char *const opt[][8], int nopts, const want *w, int *bad) {
    int order[8], orders = 0, i, j;

    for (i = 0; i < nopts; i++) order[i] = i;
    do {
        gateConfig cfg;
        args a;

        a.n = 0;
        for (i = 0; i < nopts; i++)
            for (j = 0; opt[order[i]][j] != NULL; j++)
                args_add(&a, opt[order[i]][j], strlen(opt[order[i]][j]));
        if (create_args(&cfg, &a) != GATE_OK) {
            (*bad)++;
        } else {
            if (differs(&cfg, w)[0] != '\0') (*bad)++;
            gateConfigFree(&cfg);
        }
        args_free(&a);
        orders++;
    } while (next_permutation(order, nopts));
    return orders;
}

/* "Options may come in any order". In most of the 720 orders of the
 * classifier, ALWAYSDEFER or CLASSALPHA comes before the CLASSES whose
 * labels it names. */
TEST(create_any_order) {
    int bad = 0;

    CHECK_EQ_INT(every_order(classifier_options, 6, &classifier_want, &bad), 720);
    CHECK_EQ_INT(bad, 0);
    CHECK_EQ_INT(every_order(score_options, 4, &score_want, &bad), 24);
    CHECK_EQ_INT(bad, 0);
}

/* The two commands README.md starts with. SPEC.md gives the defaults:
 * AUDIT 0.05, DELTA 0.01, nothing deferred, every label at ALPHA. */
TEST(create_defaults) {
    static const want classes = {
        .kind = GATE_KIND_CLASSIFIER, .alpha = 0.05, .delta = 0.01, .audit = 0.05, .n = 2,
        .labels = {"ignore", "respond"}, .class_alpha = {0.05, 0.05}
    };
    static const want score = {
        .kind = GATE_KIND_SCORE, .alpha = 0.05, .delta = 0.01, .audit = 0.05
    };
    gateConfig cfg;

    CREATE_OK(&cfg, "ALPHA", "0.05", "CLASSES", "2", "ignore", "respond");
    CHECK_EQ_STR(differs(&cfg, &classes), "");
    gateConfigFree(&cfg);
    CREATE_OK(&cfg, "ALPHA", "0.05", "SCORE");
    CHECK_EQ_STR(differs(&cfg, &score), "");
    gateConfigFree(&cfg);
}

TEST(create_option_words_in_any_case) {
    static const want classes = {
        .kind = GATE_KIND_CLASSIFIER, .alpha = 0.05, .delta = 0.001, .audit = 0.1, .n = 2,
        .labels = {"a", "b"}, .class_alpha = {0.01, 0.05}, .defer = {0, 1}
    };
    static const want score = {
        .kind = GATE_KIND_SCORE, .alpha = 0.1, .delta = 0.01, .audit = 0.05
    };
    gateConfig cfg;

    CREATE_OK(&cfg, "alpha", "0.05", "Classes", "2", "a", "b", "aUDIT", "0.1", "dElTa", "0.001",
              "alwaysdefer", "1", "b", "ClassAlpha", "1", "a", "0.01");
    CHECK_EQ_STR(differs(&cfg, &classes), "");
    gateConfigFree(&cfg);
    CREATE_OK(&cfg, "Alpha", "0.1", "score");
    CHECK_EQ_STR(differs(&cfg, &score), "");
    gateConfigFree(&cfg);
}

/* A counted list takes exactly its count, so its items are data whatever
 * they spell. */
TEST(create_labels_that_spell_option_words) {
    static const want w1 = {
        .kind = GATE_KIND_CLASSIFIER, .alpha = 0.05, .delta = 0.01, .audit = 0.05, .n = 3,
        .labels = {"ALPHA", "score", "audit"},
        .class_alpha = {0.01, 0.05, 0.05}, .defer = {0, 1, 0}
    };
    static const want w2 = {
        .kind = GATE_KIND_CLASSIFIER, .alpha = 0.1, .delta = 0.01, .audit = 0.05, .n = 2,
        .labels = {"CLASSES", "SCORE"}, .class_alpha = {0.1, 0.1}
    };
    static const want w3 = {
        .kind = GATE_KIND_CLASSIFIER, .alpha = 0.5, .delta = 0.01, .audit = 0.05, .n = 2,
        .labels = {"2", "0.5"}, .class_alpha = {0.5, 0.25}
    };
    gateConfig cfg;

    /* The ALPHA that sets the error rate is the last one. */
    CREATE_OK(&cfg, "CLASSES", "3", "ALPHA", "score", "audit", "CLASSALPHA", "1", "ALPHA", "0.01",
              "ALWAYSDEFER", "1", "score", "ALPHA", "0.05");
    CHECK_EQ_STR(differs(&cfg, &w1), "");
    gateConfigFree(&cfg);
    /* A label "SCORE" does not make a score gate. */
    CREATE_OK(&cfg, "CLASSES", "2", "CLASSES", "SCORE", "ALPHA", "0.1");
    CHECK_EQ_STR(differs(&cfg, &w2), "");
    gateConfigFree(&cfg);
    /* Labels may look like numbers. */
    CREATE_OK(&cfg, "ALPHA", "0.5", "CLASSES", "2", "2", "0.5", "CLASSALPHA", "1", "0.5", "0.25");
    CHECK_EQ_STR(differs(&cfg, &w3), "");
    gateConfigFree(&cfg);
    /* A list that announces one label too many swallows the next option. */
    CREATE_FAILS("unknown option '0.5'", "ALPHA", "0.1", "CLASSES", "3", "a", "b", "AUDIT", "0.5");
}

/* Labels are compared byte for byte: case matters and a prefix is another
 * label. */
TEST(create_labels_are_exact) {
    static const want w = {
        .kind = GATE_KIND_CLASSIFIER, .alpha = 0.1, .delta = 0.01, .audit = 0.05, .n = 4,
        .labels = {"spam", "Spam", "spa", "spam "},
        .class_alpha = {0.1, 0.01, 0.02, 0.1}, .defer = {0, 0, 0, 1}
    };
    gateConfig cfg;

    CREATE_OK(&cfg, "ALPHA", "0.1", "CLASSES", "4", "spam", "Spam", "spa", "spam ",
              "CLASSALPHA", "2", "Spam", "0.01", "spa", "0.02", "ALWAYSDEFER", "1", "spam ");
    CHECK_EQ_STR(differs(&cfg, &w), "");
    gateConfigFree(&cfg);
}

TEST(create_numbers) {
    gateConfig cfg;

    /* Not a number: the parser says so itself. */
    CREATE_FAILS("ALPHA must be a number", "ALPHA", "abc", "SCORE");
    CREATE_FAILS("ALPHA must be a number", "ALPHA", "0.05x", "SCORE");
    CREATE_FAILS("ALPHA must be a number", "ALPHA", "", "SCORE");
    CREATE_FAILS("ALPHA must be a number", "ALPHA", " 0.05", "SCORE");
    CREATE_FAILS("ALPHA must be a number", "ALPHA", "nan", "SCORE");
    CREATE_FAILS("ALPHA must be a number", "ALPHA", "0x1p-4", "SCORE");
    CREATE_FAILS("DELTA must be a number", SCORE_GATE, "DELTA", "1e999");
    CREATE_FAILS("AUDIT must be a number", SCORE_GATE, "AUDIT", "inf");

    /* SPEC.md: 0 < alpha < 1, 0 < delta < 1, 0 < rate <= 1. */
    CREATE_INVALID("ALPHA", "ALPHA", "0", "SCORE");
    CREATE_INVALID("ALPHA", "ALPHA", "1", "SCORE");
    CREATE_INVALID("ALPHA", "ALPHA", "-0.05", "SCORE");
    CREATE_INVALID("ALPHA", "ALPHA", "1.5", "SCORE");
    CREATE_INVALID("ALPHA", "ALPHA", "1e-999", "SCORE"); /* rounds to 0 */
    CREATE_INVALID("DELTA", SCORE_GATE, "DELTA", "0");
    CREATE_INVALID("DELTA", SCORE_GATE, "DELTA", "1");
    CREATE_INVALID("DELTA", SCORE_GATE, "DELTA", "-0.01");
    CREATE_INVALID("AUDIT", SCORE_GATE, "AUDIT", "0");
    CREATE_INVALID("AUDIT", SCORE_GATE, "AUDIT", "1.0000001");
    CREATE_INVALID("AUDIT", SCORE_GATE, "AUDIT", "-1");

    /* Just inside those ranges, with AUDIT on its closed end. */
    CREATE_OK(&cfg, "ALPHA", "0.999999", "SCORE", "DELTA", "0.999999", "AUDIT", "1");
    CHECK(cfg.alpha == 0.999999 && cfg.delta == 0.999999 && cfg.audit == 1.0);
    gateConfigFree(&cfg);
    CREATE_OK(&cfg, "ALPHA", "1e-9", "SCORE", "DELTA", "1e-12", "AUDIT", "1e-6");
    CHECK(cfg.alpha == 1e-9 && cfg.delta == 1e-12 && cfg.audit == 1e-6);
    gateConfigFree(&cfg);
}

TEST(create_option_twice) {
    CREATE_FAILS("ALPHA given twice", SCORE_GATE, "ALPHA", "0.1");
    CREATE_FAILS("ALPHA given twice", "ALPHA", "0.1", "alpha", "0.2", "SCORE");
    CREATE_FAILS("DELTA given twice", SCORE_GATE, "DELTA", "0.01", "DELTA", "0.01");
    CREATE_FAILS("AUDIT given twice", "AUDIT", "0.5", SCORE_GATE, "Audit", "0.5");
    CREATE_FAILS("SCORE given twice", SCORE_GATE, "SCORE");
    CREATE_FAILS("CLASSES given twice", AB_GATE, "CLASSES", "2", "c", "d");
    CREATE_FAILS("ALWAYSDEFER given twice",
                 AB_GATE, "ALWAYSDEFER", "1", "a", "ALWAYSDEFER", "1", "b");
    CREATE_FAILS("CLASSALPHA given twice",
                 AB_GATE, "CLASSALPHA", "1", "a", "0.05", "CLASSALPHA", "1", "b", "0.05");
}

TEST(create_unknown_option) {
    char word[80], msg[120];
    gateConfig cfg;
    args a;

    CREATE_FAILS("unknown option 'BOGUS'", SCORE_GATE, "BOGUS");
    CREATE_FAILS("unknown option 'ALPH'", "ALPH", "0.1", "SCORE");
    CREATE_FAILS("unknown option 'ALPHAS'", "ALPHAS", "0.1", "SCORE");
    CREATE_FAILS("unknown option '0.1'", "0.1", "SCORE");
    CREATE_FAILS("unknown option 'TEXT'", SCORE_GATE, "TEXT", "hello");
    CREATE_FAILS("unknown option '1'", SCORE_GATE, "1"); /* SCORE takes nothing */
    /* A list ends with its count: one item more is not an item. */
    CREATE_FAILS("unknown option 'c'", AB_GATE, "c");
    CREATE_FAILS("unknown option 'b'", AB_GATE, "ALWAYSDEFER", "1", "a", "b");
    CREATE_FAILS("unknown option 'b'", AB_GATE, "CLASSALPHA", "1", "a", "0.05", "b", "0.05");

    /* The message quotes what a label could be: UTF-8, up to 64 bytes. */
    CREATE_FAILS("unknown option 'caf\xc3\xa9'", SCORE_GATE, "caf\xc3\xa9");
    snprintf(msg, sizeof(msg), "unknown option '%s'", repeat(word, 'x', 64));
    CREATE_FAILS(msg, SCORE_GATE, word);
    /* Anything else must not reach a reply. */
    CREATE_FAILS("unknown option '?'", SCORE_GATE, repeat(word, 'x', 65));
    CREATE_FAILS("unknown option '?'", SCORE_GATE, "");
    CREATE_FAILS("unknown option '?'", SCORE_GATE, "\xff\xfe");
    CREATE_FAILS("unknown option '?'", SCORE_GATE, "x\r\n-ERR forged");
    args_set(&a, "ALPHA", "0.1", NULL);
    args_add(&a, LIT("SCORE\0"));
    CHECK(refused(create_args(&cfg, &a), &cfg));
    CHECK_EQ_STR(g_err, "unknown option '?'");
    args_free(&a);
}

#define COUNT_MSG(option) option " count must be a positive integer"
#define SHORT_MSG(option) option " count exceeds the arguments that follow"

/* Every option, cut off at every point. */
TEST(create_truncated) {
    CREATE_FAILS("ALPHA needs an argument", "ALPHA");
    CREATE_FAILS("ALPHA needs an argument", "SCORE", "ALPHA");
    CREATE_FAILS("DELTA needs an argument", SCORE_GATE, "DELTA");
    CREATE_FAILS("AUDIT needs an argument", SCORE_GATE, "AUDIT");
    CREATE_FAILS("CLASSES needs an argument", "ALPHA", "0.1", "CLASSES");
    CREATE_FAILS("ALWAYSDEFER needs an argument", AB_GATE, "ALWAYSDEFER");
    CREATE_FAILS("CLASSALPHA needs an argument", AB_GATE, "CLASSALPHA");
    /* The next option is taken for the missing number, and is not one. */
    CREATE_FAILS("ALPHA must be a number", "ALPHA", "SCORE");

    CREATE_FAILS(SHORT_MSG("CLASSES"), "ALPHA", "0.1", "CLASSES", "2");
    CREATE_FAILS(SHORT_MSG("CLASSES"), "ALPHA", "0.1", "CLASSES", "2", "a");
    CREATE_FAILS(SHORT_MSG("CLASSES"), "ALPHA", "0.1", "CLASSES", "3", "a", "b");
    CREATE_FAILS(SHORT_MSG("ALWAYSDEFER"), AB_GATE, "ALWAYSDEFER", "1");
    CREATE_FAILS(SHORT_MSG("ALWAYSDEFER"), AB_GATE, "ALWAYSDEFER", "2", "a");
    CREATE_FAILS(SHORT_MSG("CLASSALPHA"), AB_GATE, "CLASSALPHA", "1");
    CREATE_FAILS(SHORT_MSG("CLASSALPHA"), AB_GATE, "CLASSALPHA", "1", "a"); /* no alpha */
    CREATE_FAILS(SHORT_MSG("CLASSALPHA"), AB_GATE, "CLASSALPHA", "2", "a", "0.05", "b");
}

TEST(create_count) {
    static const char *const bad[] = {
        "0", "x", "-2", "+2", "2.0", "2e0", "", " 2", "0x2",
        "18446744073709551616" /* 2^64 */
    };
    size_t i;

    for (i = 0; i < COUNTOF(bad); i++) {
        CREATE_FAILS(COUNT_MSG("CLASSES"), "ALPHA", "0.1", "CLASSES", bad[i], "a", "b");
        CREATE_FAILS(COUNT_MSG("ALWAYSDEFER"), AB_GATE, "ALWAYSDEFER", bad[i], "a");
        CREATE_FAILS(COUNT_MSG("CLASSALPHA"), AB_GATE, "CLASSALPHA", bad[i], "a", "0.05");
    }

    /* Counts that become small again when narrowed or doubled carelessly:
     * 2^32 + 2 is 2 in 32 bits, and twice 2^63 is 0 in 64. */
    CREATE_FAILS(SHORT_MSG("CLASSES"), "ALPHA", "0.1", "CLASSES", "4294967298", "a", "b");
    CREATE_FAILS(SHORT_MSG("CLASSES"),
                 "ALPHA", "0.1", "CLASSES", "18446744073709551615", "a", "b");
    CREATE_FAILS(SHORT_MSG("ALWAYSDEFER"), AB_GATE, "ALWAYSDEFER", "4294967297", "a");
    CREATE_FAILS(SHORT_MSG("CLASSALPHA"), AB_GATE, "CLASSALPHA", "4294967297", "a", "0.05");
    CREATE_FAILS(SHORT_MSG("CLASSALPHA"),
                 AB_GATE, "CLASSALPHA", "9223372036854775808", "a", "0.05");
}

TEST(create_alpha_is_required) {
    gateConfig cfg;

    memset(&cfg, 0xff, sizeof(cfg));
    g_err[0] = '\0';
    CHECK(refused(gateParseCreate(NULL, 0, SNAPSHOT, TIMEOUT, &cfg, g_err, sizeof(g_err)), &cfg));
    CHECK_EQ_STR(g_err, "ALPHA is required");
    CREATE_FAILS("ALPHA is required", "SCORE");
    CREATE_FAILS("ALPHA is required", "SCORE", "DELTA", "0.01", "AUDIT", "0.05");
    CREATE_FAILS("ALPHA is required", "CLASSES", "2", "a", "b");
    /* ALPHA as a label is not the option. */
    CREATE_FAILS("ALPHA is required", "CLASSES", "2", "ALPHA", "0.1");
}

TEST(create_classes_or_score) {
    CREATE_FAILS("exactly one of CLASSES and SCORE is required", "ALPHA", "0.1");
    CREATE_FAILS("exactly one of CLASSES and SCORE is required",
                 "ALPHA", "0.1", "AUDIT", "0.5", "DELTA", "0.1");
    CREATE_FAILS("exactly one of CLASSES and SCORE is required", AB_GATE, "SCORE");
    CREATE_FAILS("exactly one of CLASSES and SCORE is required", "score", AB_GATE);
}

TEST(create_lists_need_classes) {
    CREATE_FAILS("ALWAYSDEFER needs CLASSES", SCORE_GATE, "ALWAYSDEFER", "1", "a");
    CREATE_FAILS("ALWAYSDEFER needs CLASSES", "ALWAYSDEFER", "1", "a", SCORE_GATE);
    CREATE_FAILS("CLASSALPHA needs CLASSES", SCORE_GATE, "CLASSALPHA", "1", "a", "0.05");
    /* With neither CLASSES nor SCORE, this is the more useful complaint. */
    CREATE_FAILS("ALWAYSDEFER needs CLASSES", "ALPHA", "0.1", "ALWAYSDEFER", "1", "a");
    CREATE_FAILS("CLASSALPHA needs CLASSES", "ALPHA", "0.1", "CLASSALPHA", "1", "a", "0.05");
}

/* GCREATE ALPHA 0.1 CLASSES n c0 c1 ... */
static int create_classes(gateConfig *cfg, int n) {
    char word[16];
    args a;
    int i, rc;

    args_set(&a, "ALPHA", "0.1", "CLASSES", NULL);
    snprintf(word, sizeof(word), "%d", n);
    args_add(&a, word, strlen(word));
    for (i = 0; i < n; i++) {
        snprintf(word, sizeof(word), "c%d", i);
        args_add(&a, word, strlen(word));
    }
    rc = create_args(cfg, &a);
    args_free(&a);
    return rc;
}

/* SPEC.md: "the 2 to 64 labels the teacher can answer". */
TEST(create_number_of_classes) {
    gateConfig cfg;
    uint32_t i, bad = 0;

    CREATE_FAILS("CLASSES must list between 2 and 64 labels", "ALPHA", "0.1", "CLASSES", "1", "a");
    CHECK(refused(create_classes(&cfg, 65), &cfg));
    CHECK_EQ_STR(g_err, "CLASSES must list between 2 and 64 labels");

    CHECK(create_classes(&cfg, 2) == GATE_OK && cfg.nclasses == 2);
    gateConfigFree(&cfg);
    CHECK(create_classes(&cfg, 64) == GATE_OK && cfg.nclasses == 64);
    if (cfg.nclasses == 64) {
        CHECK_EQ_STR(cfg.labels[0], "c0");
        CHECK_EQ_STR(cfg.labels[9], "c9");
        CHECK_EQ_STR(cfg.labels[63], "c63");
        for (i = 0; i < 64; i++)
            if (cfg.class_alpha[i] != 0.1 || cfg.class_defer[i] != 0) bad++;
        CHECK_EQ_INT(bad, 0);
    }
    gateConfigFree(&cfg);
}

#define LABEL_MSG "labels must be 1 to 64 bytes of UTF-8 without control characters"

/* GCREATE ALPHA 0.1 CLASSES 2 good <label> */
static int create_label(gateConfig *cfg, const char *label, size_t len) {
    args a;
    int rc;

    args_set(&a, "ALPHA", "0.1", "CLASSES", "2", "good", NULL);
    args_add(&a, label, len);
    rc = create_args(cfg, &a);
    args_free(&a);
    return rc;
}

/* SPEC.md: "Labels are 1 to 64 bytes of UTF-8 without control characters." */
TEST(create_labels) {
    static const bytes good[] = {
        {LIT("caf\xc3\xa9")},                             /* U+00E9 */
        {LIT("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e")},    /* three CJK characters */
        {LIT("\xf0\x9f\x98\x80")},                        /* U+1F600 */
        {LIT("\xc2\xa0")},                                /* U+00A0, the first after C1 */
        {LIT("two words")}, {LIT("a-b_c.d:e/f")}, {LIT("~")}, {LIT(" ")}
    };
    static const bytes bad[] = {
        {LIT("")},
        {LIT("a\nb")}, {LIT("a\rb")}, {LIT("tab\t")}, {LIT("\x1b[31m")}, {LIT("\x1f")},
        {LIT("del\x7f")},
        /* NUL is a control character. Cut at it, each of these would be a
         * fine label. */
        {LIT("a\0b")}, {LIT("fine\0")},
        {LIT("\xc2\x80")}, {LIT("\xc2\x9f")},             /* C1 controls */
        {LIT("\xff")}, {LIT("\x80")}, {LIT("caf\xc3")}, {LIT("\xe6\x97")},
        {LIT("\xc0\xaf")}, {LIT("\xe0\x80\xaf")},         /* overlong '/' */
        {LIT("\xed\xa0\x80")},                            /* a surrogate */
        {LIT("\xf4\x90\x80\x80")}                         /* above U+10FFFF */
    };
    char longest[80];
    gateConfig cfg;
    size_t i;

    for (i = 0; i < COUNTOF(good); i++) {
        int rc = create_label(&cfg, good[i].s, good[i].len);

        CHECK_ON(good[i].s, rc == GATE_OK && strlen(cfg.labels[1]) == good[i].len &&
                            memcmp(cfg.labels[1], good[i].s, good[i].len) == 0);
        if (rc == GATE_OK) gateConfigFree(&cfg);
    }
    for (i = 0; i < COUNTOF(bad); i++) {
        CHECK_ON(bad[i].s, refused(create_label(&cfg, bad[i].s, bad[i].len), &cfg) &&
                           strcmp(g_err, LABEL_MSG) == 0);
    }

    repeat(longest, 'x', 64);
    CHECK(create_label(&cfg, longest, 64) == GATE_OK && strcmp(cfg.labels[1], longest) == 0);
    gateConfigFree(&cfg);
    repeat(longest, 'x', 65);
    CHECK(refused(create_label(&cfg, longest, 65), &cfg));
    CHECK_EQ_STR(g_err, LABEL_MSG);
}

TEST(create_duplicate_labels) {
    gateConfig cfg;

    CREATE_INVALID("label", "ALPHA", "0.1", "CLASSES", "2", "a", "a");
    CREATE_INVALID("label", "ALPHA", "0.1", "CLASSES", "3", "a", "b", "a");
    /* Not the same bytes, not a duplicate. */
    CREATE_OK(&cfg, "ALPHA", "0.1", "CLASSES", "3", "a", "b", "A");
    CHECK_EQ_INT(cfg.nclasses, 3);
    gateConfigFree(&cfg);
}

TEST(create_defer_every_label) {
    gateConfig cfg;

    CREATE_INVALID("ALWAYSDEFER", AB_GATE, "ALWAYSDEFER", "2", "a", "b");
    CREATE_INVALID("ALWAYSDEFER", "ALWAYSDEFER", "2", "b", "a", AB_GATE);
    /* All but one is fine. */
    CREATE_OK(&cfg, "ALPHA", "0.1", "CLASSES", "3", "a", "b", "c", "ALWAYSDEFER", "2", "c", "a");
    CHECK(cfg.class_defer[0] == 1 && cfg.class_defer[1] == 0 && cfg.class_defer[2] == 1);
    gateConfigFree(&cfg);
}

TEST(create_unknown_label) {
    gateConfig cfg;
    args a;

    CREATE_FAILS("unknown label 'c' in ALWAYSDEFER", AB_GATE, "ALWAYSDEFER", "1", "c");
    CREATE_FAILS("unknown label 'c' in ALWAYSDEFER", AB_GATE, "ALWAYSDEFER", "2", "a", "c");
    CREATE_FAILS("unknown label 'c' in CLASSALPHA", AB_GATE, "CLASSALPHA", "1", "c", "0.05");
    CREATE_FAILS("unknown label 'c' in CLASSALPHA",
                 AB_GATE, "CLASSALPHA", "2", "a", "0.05", "c", "0.05");
    /* Named before CLASSES is read, it is still looked up in CLASSES. */
    CREATE_FAILS("unknown label 'c' in ALWAYSDEFER", "ALWAYSDEFER", "1", "c", AB_GATE);
    /* Byte for byte. */
    CREATE_FAILS("unknown label 'A' in ALWAYSDEFER", AB_GATE, "ALWAYSDEFER", "1", "A");
    CREATE_FAILS("unknown label 'ab' in CLASSALPHA", AB_GATE, "CLASSALPHA", "1", "ab", "0.05");
    CREATE_FAILS("unknown label 'a ' in ALWAYSDEFER", AB_GATE, "ALWAYSDEFER", "1", "a ");
    /* What could not be a label is not quoted. */
    CREATE_FAILS("unknown label '?' in ALWAYSDEFER", AB_GATE, "ALWAYSDEFER", "1", "\xff");
    CREATE_FAILS("unknown label '?' in CLASSALPHA", AB_GATE, "CLASSALPHA", "1", "", "0.05");
    /* "a" followed by a NUL is not "a". */
    args_set(&a, AB_GATE, "ALWAYSDEFER", "1", NULL);
    args_add(&a, LIT("a\0"));
    CHECK(refused(create_args(&cfg, &a), &cfg));
    CHECK_EQ_STR(g_err, "unknown label '?' in ALWAYSDEFER");
    args_free(&a);
}

TEST(create_label_twice_in_a_list) {
    CREATE_FAILS("label 'a' given twice in ALWAYSDEFER", AB_GATE, "ALWAYSDEFER", "2", "a", "a");
    CREATE_FAILS("label 'b' given twice in ALWAYSDEFER", "ALWAYSDEFER", "3", "a", "b", "b",
                 "ALPHA", "0.1", "CLASSES", "4", "a", "b", "c", "d");
    CREATE_FAILS("label 'a' given twice in CLASSALPHA",
                 AB_GATE, "CLASSALPHA", "2", "a", "0.05", "a", "0.01");
    /* With the same alpha as well, and when the first one was ALPHA. */
    CREATE_FAILS("label 'a' given twice in CLASSALPHA",
                 AB_GATE, "CLASSALPHA", "2", "a", "0.05", "a", "0.05");
    CREATE_FAILS("label 'b' given twice in CLASSALPHA",
                 AB_GATE, "CLASSALPHA", "3", "b", "0.1", "a", "0.05", "b", "0.05");
}

TEST(create_label_in_both_lists) {
    CREATE_FAILS("label 'a' is in both ALWAYSDEFER and CLASSALPHA",
                 AB_GATE, "ALWAYSDEFER", "1", "a", "CLASSALPHA", "1", "a", "0.05");
    CREATE_FAILS("label 'a' is in both ALWAYSDEFER and CLASSALPHA",
                 AB_GATE, "CLASSALPHA", "1", "a", "0.05", "ALWAYSDEFER", "1", "a");
    CREATE_FAILS("label 'c' is in both ALWAYSDEFER and CLASSALPHA",
                 "CLASSALPHA", "2", "a", "0.05", "c", "0.05", "ALWAYSDEFER", "2", "b", "c",
                 "ALPHA", "0.1", "CLASSES", "3", "a", "b", "c");
}

#define CLASSALPHA_MSG "CLASSALPHA for 'a' must be greater than 0 and at most ALPHA"

/* SPEC.md: "a stricter error rate for some labels (must be <= ALPHA)". */
TEST(create_class_alpha_value) {
    gateConfig cfg;

    CREATE_FAILS(CLASSALPHA_MSG, AB_GATE, "CLASSALPHA", "1", "a", "abc");
    CREATE_FAILS(CLASSALPHA_MSG, AB_GATE, "CLASSALPHA", "1", "a", "");
    CREATE_FAILS(CLASSALPHA_MSG, AB_GATE, "CLASSALPHA", "1", "a", "0.05 ");
    CREATE_FAILS(CLASSALPHA_MSG, AB_GATE, "CLASSALPHA", "1", "a", "nan");
    CREATE_FAILS(CLASSALPHA_MSG, AB_GATE, "CLASSALPHA", "1", "a", "inf");
    CREATE_FAILS(CLASSALPHA_MSG, AB_GATE, "CLASSALPHA", "1", "a", "-0.05");
    /* Zero must be refused, not read as "no CLASSALPHA for this label". */
    CREATE_FAILS(CLASSALPHA_MSG, AB_GATE, "CLASSALPHA", "1", "a", "0");
    CREATE_FAILS(CLASSALPHA_MSG, AB_GATE, "CLASSALPHA", "1", "a", "0.0");
    CREATE_FAILS(CLASSALPHA_MSG, AB_GATE, "CLASSALPHA", "1", "a", "1e-999");
    /* Above the ALPHA of 0.1. */
    CREATE_INVALID("CLASSALPHA", AB_GATE, "CLASSALPHA", "1", "a", "0.11");
    CREATE_INVALID("CLASSALPHA", AB_GATE, "CLASSALPHA", "1", "a", "1");

    /* ALPHA itself is allowed, and so is anything positive below it. */
    CREATE_OK(&cfg, AB_GATE, "CLASSALPHA", "1", "a", "0.1");
    CHECK(cfg.class_alpha[0] == 0.1 && cfg.class_alpha[1] == 0.1);
    gateConfigFree(&cfg);
    CREATE_OK(&cfg, AB_GATE, "CLASSALPHA", "1", "a", "1e-300");
    CHECK(cfg.class_alpha[0] == 1e-300 && cfg.class_alpha[1] == 0.1);
    gateConfigFree(&cfg);
}

/* gate.snapshot-examples and gate.feedback-timeout are stored as given and
 * checked like the rest of the config. */
TEST(create_server_values) {
    gateConfig cfg;
    args a;

    args_set(&a, SCORE_GATE, NULL);
    CHECK(gateParseCreate(a.v, a.n, 1, 1, &cfg, g_err, sizeof(g_err)) == GATE_OK);
    CHECK(cfg.snapshot_examples == 1 && cfg.feedback_timeout_ms == 1);
    gateConfigFree(&cfg);
    /* The largest of each: any uint32, and 7 days in milliseconds. */
    CHECK(gateParseCreate(a.v, a.n, UINT32_MAX, 604800000, &cfg, g_err, sizeof(g_err)) == GATE_OK);
    CHECK(cfg.snapshot_examples == UINT32_MAX && cfg.feedback_timeout_ms == 604800000);
    gateConfigFree(&cfg);

    CHECK(refused(gateParseCreate(a.v, a.n, 0, 60000, &cfg, g_err, sizeof(g_err)), &cfg));
    CHECK(refused(gateParseCreate(a.v, a.n, 1000, 0, &cfg, g_err, sizeof(g_err)), &cfg));
    CHECK(refused(gateParseCreate(a.v, a.n, 1000, -60000, &cfg, g_err, sizeof(g_err)), &cfg));
    CHECK(refused(gateParseCreate(a.v, a.n, 1000, 604800001, &cfg, g_err, sizeof(g_err)), &cfg));
    args_free(&a);
}

/* The message is optional and never overruns its buffer. */
TEST(create_error_buffer) {
    gateConfig cfg;
    char *small;
    args a;

    args_set(&a, SCORE_GATE, "BOGUS", NULL);
    CHECK(refused(gateParseCreate(a.v, a.n, SNAPSHOT, TIMEOUT, &cfg, NULL, 0), &cfg));
    CHECK(refused(gateParseCreate(a.v, a.n, SNAPSHOT, TIMEOUT, &cfg, NULL, 100), &cfg));
    small = malloc(8);
    memset(small, 'x', 8);
    CHECK(refused(gateParseCreate(a.v, a.n, SNAPSHOT, TIMEOUT, &cfg, small, 0), &cfg));
    CHECK(small[0] == 'x');
    CHECK(refused(gateParseCreate(a.v, a.n, SNAPSHOT, TIMEOUT, &cfg, small, 8), &cfg));
    CHECK(memcmp(small, "unknown", 8) == 0); /* the start of it, terminated */
    free(small);
    args_free(&a);
}

/* Allocations that fail once a budget is spent. */
static int alloc_budget = -1; /* -1: no limit */

static int alloc_allowed(void) {
    if (alloc_budget < 0) return 1;
    if (alloc_budget == 0) return 0;
    alloc_budget--;
    return 1;
}

static void *budget_malloc(size_t n) {
    return alloc_allowed() ? malloc(n) : NULL;
}

static void *budget_calloc(size_t n, size_t size) {
    return alloc_allowed() ? calloc(n, size) : NULL;
}

static void *budget_realloc(void *p, size_t n) {
    return alloc_allowed() ? realloc(p, n) : NULL;
}

/* Whichever allocation fails, the parse fails cleanly: valgrind and
 * AddressSanitizer see to it that nothing is leaked on the way out. */
TEST(create_out_of_memory) {
    static const want w = {
        .kind = GATE_KIND_CLASSIFIER, .alpha = 0.1, .delta = 0.01, .audit = 0.05, .n = 2,
        .labels = {"a", "b"}, .class_alpha = {0.02, 0.1}, .defer = {0, 1}
    };
    gateConfig cfg;
    int budget, failures = 0, rc = GATE_ERR;

    gateSetAllocator(budget_malloc, budget_calloc, budget_realloc, free);
    for (budget = 0; budget < 64; budget++) {
        alloc_budget = budget;
        rc = create(&cfg, AB_GATE, "ALWAYSDEFER", "1", "b", "CLASSALPHA", "1", "a", "0.02", NULL);
        if (rc == GATE_OK) break;
        failures++;
        CHECK(zeroed(&cfg));
        CHECK_EQ_STR(g_err, "out of memory");
    }
    alloc_budget = -1;
    gateSetAllocator(malloc, calloc, realloc, free);

    CHECK(failures > 0); /* a classifier config cannot be built without memory */
    CHECK(rc == GATE_OK);
    if (rc == GATE_OK) {
        CHECK_EQ_STR(differs(&cfg, &w), "");
        gateConfigFree(&cfg);
    }
}

/* A valid command with one to three random edits. Whatever comes out, the
 * parser either gives a config that is valid or refuses with a zeroed
 * config and a message that is one line of printable text. Under the
 * sanitizers this is also where a malformed command would be caught
 * reading out of bounds or leaking. */
TEST(create_survives_edits) {
    static const char *const base[] = {
        "ALPHA", "0.05", "CLASSES", "3", "billing", "bug", "other", "AUDIT", "0.1",
        "DELTA", "0.001", "ALWAYSDEFER", "1", "other",
        "CLASSALPHA", "2", "billing", "0.01", "bug", "0.02"
    };
    static const bytes words[] = {
        {LIT("ALPHA")}, {LIT("delta")}, {LIT("AUDIT")}, {LIT("SCORE")}, {LIT("CLASSES")},
        {LIT("ALWAYSDEFER")}, {LIT("CLASSALPHA")}, {LIT("0")}, {LIT("1")}, {LIT("2")},
        {LIT("3")}, {LIT("64")}, {LIT("0.01")}, {LIT("0.5")}, {LIT("1e999")},
        {LIT("18446744073709551616")}, {LIT("billing")}, {LIT("bug")}, {LIT("other")},
        {LIT("new")}, {LIT("")}, {LIT("\xff")}, {LIT("a\nb")}, {LIT("a\0b")}
    };
    uint64_t rng = 20261128;
    int iter, ok = 0, bad = 0;

    for (iter = 0; iter < 4000; iter++) {
        bytes cmd[COUNTOF(base) + 3];
        size_t n = COUNTOF(base), i;
        int edits = 1 + (int)(gate_test_rand64(&rng) % 3);
        gateConfig cfg;
        args a;

        for (i = 0; i < n; i++) {
            cmd[i].s = base[i];
            cmd[i].len = strlen(base[i]);
        }
        while (edits-- > 0) {
            size_t at = gate_test_rand64(&rng) % n;
            const bytes *w = &words[gate_test_rand64(&rng) % COUNTOF(words)];

            switch (gate_test_rand64(&rng) % 3) {
            case 0: /* replace */
                cmd[at] = *w;
                break;
            case 1: /* delete */
                memmove(&cmd[at], &cmd[at + 1], (n - at - 1) * sizeof(cmd[0]));
                n--;
                break;
            default: /* insert */
                memmove(&cmd[at + 1], &cmd[at], (n - at) * sizeof(cmd[0]));
                cmd[at] = *w;
                n++;
            }
        }

        a.n = 0;
        for (i = 0; i < n; i++) args_add(&a, cmd[i].s, cmd[i].len);
        if (create_args(&cfg, &a) == GATE_OK) {
            char why[64];

            ok++;
            if (cfg.snapshot_examples != SNAPSHOT || cfg.feedback_timeout_ms != TIMEOUT ||
                gateConfigValidate(&cfg, why, sizeof(why)) != GATE_OK)
                bad++;
            gateConfigFree(&cfg);
        } else if (!zeroed(&cfg) || !gateValidName(g_err, strlen(g_err), sizeof(g_err))) {
            bad++;
        }
        args_free(&a);
    }
    CHECK_EQ_INT(bad, 0);
    CHECK(ok > 0 && ok < 4000); /* the edits are mild enough for both outcomes */
}

/* --------------------------------- Requests ------------------------------ */

/* 'req' starts as garbage, like the config of a GCREATE. */
static int request(const args *a, int *pos, gateRequest *req) {
    gateArg *v = args_exact(a);
    int rc;

    memset(req, 0xff, sizeof(*req));
    g_err[0] = '\0';
    rc = gateParseRequest(v, a->n, pos, req, g_err, sizeof(g_err));
    free(v);
    return rc;
}

static int empty(const gateRequest *req) {
    return req->text == NULL && req->text_len == 0 && req->nfeatures == 0;
}

/* The request is refused with exactly this message: it is left empty and
 * the position where it started. */
#define REQUEST_FAILS(msg, ...) do { \
    gateRequest req_; \
    args args_; \
    int pos_ = 0; \
    args_set(&args_, __VA_ARGS__, NULL); \
    CHECK(request(&args_, &pos_, &req_) == GATE_ERR && pos_ == 0 && empty(&req_)); \
    CHECK_EQ_STR(g_err, msg); \
    args_free(&args_); \
} while (0)

TEST(request_text_only) {
    gateRequest req;
    args a;
    int pos = 0;

    args_set(&a, "TEXT", "where is my invoice?", NULL);
    CHECK(request(&a, &pos, &req) == GATE_OK);
    CHECK_EQ_INT(pos, 2);
    CHECK(req.text == a.v[1].ptr); /* borrowed, not copied */
    CHECK_EQ_INT(req.text_len, 20);
    CHECK_EQ_INT(req.nfeatures, 0);
    args_free(&a);
}

/* The example of SPEC.md: FEATURES 2 mentioned 1 thread_depth 0.3 */
TEST(request_text_and_features) {
    gateRequest req;
    args a;
    int pos = 0;

    args_set(&a, "text", "ping", "features", "2", "mentioned", "1", "thread_depth", "0.3", NULL);
    CHECK(request(&a, &pos, &req) == GATE_OK);
    CHECK_EQ_INT(pos, 8);
    CHECK(req.text == a.v[1].ptr);
    CHECK_EQ_INT(req.text_len, 4);
    CHECK_EQ_INT(req.nfeatures, 2);
    CHECK(req.features[0].name == a.v[4].ptr);
    CHECK_EQ_INT(req.features[0].name_len, 9);
    CHECK(req.features[0].value == 1.0);
    CHECK(req.features[1].name == a.v[6].ptr);
    CHECK_EQ_INT(req.features[1].name_len, 12);
    CHECK(req.features[1].value == 0.3);
    args_free(&a);
}

/* The parser takes its part of a longer command and says where it ends. */
TEST(request_stops_at_the_next_argument) {
    gateRequest req;
    args a;
    int pos = 2;

    args_set(&a, "GADD", "key", "TEXT", "refund please", "FEATURES", "1", "urgent", "-1",
             "LABEL", "billing", NULL);
    CHECK(request(&a, &pos, &req) == GATE_OK);
    CHECK_EQ_INT(pos, 8);
    CHECK(gateArgIs(&a.v[pos], "LABEL"));
    CHECK(req.text == a.v[3].ptr && req.text_len == 13);
    CHECK(req.nfeatures == 1 && req.features[0].name == a.v[6].ptr);
    CHECK(req.features[0].name_len == 6 && req.features[0].value == -1.0);
    args_free(&a);

    pos = 0;
    args_set(&a, "TEXT", "refund please", "LABEL", "billing", NULL);
    CHECK(request(&a, &pos, &req) == GATE_OK);
    CHECK_EQ_INT(pos, 2);
    CHECK_EQ_INT(req.nfeatures, 0);
    args_free(&a);

    /* FEATURES 0 is still two arguments. */
    pos = 0;
    args_set(&a, "TEXT", "x", "FEATURES", "0", "LABEL", "billing", NULL);
    CHECK(request(&a, &pos, &req) == GATE_OK);
    CHECK_EQ_INT(pos, 4);
    CHECK(req.text == a.v[1].ptr && req.text_len == 1 && req.nfeatures == 0);
    args_free(&a);

    /* FEATURES takes its count of pairs, whatever they spell, and leaves
     * the rest. */
    pos = 0;
    args_set(&a, "TEXT", "x", "FEATURES", "1", "FEATURES", "2", "b", "3", NULL);
    CHECK(request(&a, &pos, &req) == GATE_OK);
    CHECK_EQ_INT(pos, 6);
    CHECK(req.nfeatures == 1 && req.features[0].name == a.v[4].ptr);
    CHECK(req.features[0].name_len == 8 && req.features[0].value == 2.0);
    args_free(&a);

    /* The text is data as well. */
    pos = 0;
    args_set(&a, "TEXT", "FEATURES", NULL);
    CHECK(request(&a, &pos, &req) == GATE_OK);
    CHECK_EQ_INT(pos, 2);
    CHECK(req.text == a.v[1].ptr && req.text_len == 8 && req.nfeatures == 0);
    args_free(&a);
}

/* TEXT is read as bytes: it may be empty, hold NULs or not be UTF-8. */
TEST(request_text_bytes) {
    char *big = malloc(GATE_MAX_TEXT_BYTES + 1);
    gateRequest req;
    args a;
    int pos = 0;

    args_set(&a, "TEXT", "", NULL);
    CHECK(request(&a, &pos, &req) == GATE_OK);
    CHECK(pos == 2 && req.text_len == 0 && req.nfeatures == 0);
    args_free(&a);

    pos = 0;
    args_set(&a, "TEXT", NULL);
    args_add(&a, LIT("a\0b\0\xff\n"));
    CHECK(request(&a, &pos, &req) == GATE_OK);
    CHECK(pos == 2 && req.text == a.v[1].ptr && req.text_len == 6);
    args_free(&a);

    /* gate_defs.h: a TEXT of up to 2^20 bytes is accepted. */
    memset(big, 'a', GATE_MAX_TEXT_BYTES + 1);
    pos = 0;
    args_set(&a, "TEXT", NULL);
    args_add(&a, big, 1048576);
    CHECK(request(&a, &pos, &req) == GATE_OK);
    CHECK(pos == 2 && req.text == a.v[1].ptr && req.text_len == 1048576);
    args_free(&a);

    pos = 0;
    args_set(&a, "TEXT", NULL);
    args_add(&a, big, 1048577);
    CHECK(request(&a, &pos, &req) == GATE_ERR && pos == 0 && empty(&req));
    CHECK_EQ_STR(g_err, "TEXT is longer than 1048576 bytes");
    args_free(&a);
    free(big);
}

TEST(request_needs_text) {
    gateRequest req;
    args a;
    int pos;

    REQUEST_FAILS("expected TEXT", "SCORE", "0.5");
    REQUEST_FAILS("expected TEXT", "FEATURES", "0");
    REQUEST_FAILS("expected TEXT", "TEXTS", "x");
    REQUEST_FAILS("expected TEXT", "LABEL", "x", "TEXT", "y");
    REQUEST_FAILS("expected TEXT", "TEXT"); /* the word without the text */

    /* Nothing, or too little, left to parse. */
    args_set(&a, "TEXT", "x", NULL);
    pos = 2;
    CHECK(request(&a, &pos, &req) == GATE_ERR && pos == 2 && empty(&req));
    CHECK_EQ_STR(g_err, "expected TEXT");
    pos = 1;
    CHECK(request(&a, &pos, &req) == GATE_ERR && pos == 1 && empty(&req));
    args_free(&a);
    /* And no room for the message. */
    pos = 0;
    CHECK(gateParseRequest(NULL, 0, &pos, &req, NULL, 0) == GATE_ERR && pos == 0 && empty(&req));
}

#define FEATURES_COUNT_MSG "FEATURES count must be an integer between 0 and 64"

TEST(request_feature_count) {
    gateRequest req;
    args a;
    int pos = 0;

    REQUEST_FAILS(FEATURES_COUNT_MSG, "TEXT", "x", "FEATURES");
    REQUEST_FAILS(FEATURES_COUNT_MSG, "TEXT", "x", "FEATURES", "one", "a", "1");
    REQUEST_FAILS(FEATURES_COUNT_MSG, "TEXT", "x", "FEATURES", "-1", "a", "1");
    REQUEST_FAILS(FEATURES_COUNT_MSG, "TEXT", "x", "FEATURES", "1.0", "a", "1");
    REQUEST_FAILS(FEATURES_COUNT_MSG, "TEXT", "x", "FEATURES", "", "a", "1");
    REQUEST_FAILS(FEATURES_COUNT_MSG, "TEXT", "x", "FEATURES", "4294967297", "a", "1");
    REQUEST_FAILS(FEATURES_COUNT_MSG, "TEXT", "x", "FEATURES", "18446744073709551616", "a", "1");

    /* More pairs announced than given. */
    REQUEST_FAILS(SHORT_MSG("FEATURES"), "TEXT", "x", "FEATURES", "1");
    REQUEST_FAILS(SHORT_MSG("FEATURES"), "TEXT", "x", "FEATURES", "1", "a");
    REQUEST_FAILS(SHORT_MSG("FEATURES"), "TEXT", "x", "FEATURES", "2", "a", "1");
    REQUEST_FAILS(SHORT_MSG("FEATURES"), "TEXT", "x", "FEATURES", "2", "a", "1", "b");

    /* Fewer: the rest is the caller's to refuse. */
    args_set(&a, "TEXT", "x", "FEATURES", "1", "a", "1", "b", "2", NULL);
    CHECK(request(&a, &pos, &req) == GATE_OK);
    CHECK_EQ_INT(pos, 6);
    CHECK_EQ_INT(req.nfeatures, 1);
    args_free(&a);
}

/* TEXT x FEATURES count f0 0 f1 1 ..., with 'pairs' pairs. */
static void features_command(args *a, int count, int pairs) {
    char word[16];
    int i;

    args_set(a, "TEXT", "x", "FEATURES", NULL);
    snprintf(word, sizeof(word), "%d", count);
    args_add(a, word, strlen(word));
    for (i = 0; i < pairs; i++) {
        snprintf(word, sizeof(word), "f%d", i);
        args_add(a, word, strlen(word));
        snprintf(word, sizeof(word), "%d", i);
        args_add(a, word, strlen(word));
    }
}

/* gate_defs.h: at most 64 named features per request. */
TEST(request_most_features) {
    gateRequest req;
    args a;
    int pos = 0, bad = 0, i;

    features_command(&a, 64, 64);
    CHECK(request(&a, &pos, &req) == GATE_OK);
    CHECK_EQ_INT(pos, 4 + 2 * 64);
    CHECK_EQ_INT(req.nfeatures, 64);
    for (i = 0; i < 64 && req.nfeatures == 64; i++) {
        const gateNamedFeature *f = &req.features[i];

        if (f->name != a.v[4 + 2 * i].ptr || f->name_len != a.v[4 + 2 * i].len || f->value != i)
            bad++;
    }
    CHECK_EQ_INT(bad, 0);
    args_free(&a);

    pos = 0;
    features_command(&a, 65, 65);
    CHECK(request(&a, &pos, &req) == GATE_ERR && pos == 0 && empty(&req));
    CHECK_EQ_STR(g_err, FEATURES_COUNT_MSG);
    args_free(&a);
}

TEST(request_feature_values) {
    static const char *const bad[] = {
        "abc", "", "nan", "NaN", "inf", "-inf", "infinity", "1e999", "-1e999", "0x1", " 1", "1 "
    };
    gateRequest req;
    args a;
    size_t i;
    int pos = 0;

    /* Any finite number, of any size: clamping is the featurizer's part. */
    args_set(&a, "TEXT", "x", "FEATURES", "5", "a", "1e300", "b", "-12345.5", "c", "0",
             "d", ".5", "e", "-1e-300", NULL);
    CHECK(request(&a, &pos, &req) == GATE_OK);
    CHECK_EQ_INT(req.nfeatures, 5);
    CHECK(req.features[0].value == 1e300 && req.features[1].value == -12345.5);
    CHECK(req.features[2].value == 0.0 && req.features[3].value == 0.5);
    CHECK(req.features[4].value == -1e-300);
    args_free(&a);

    for (i = 0; i < COUNTOF(bad); i++)
        REQUEST_FAILS("the value of feature 'a' must be a finite number",
                      "TEXT", "x", "FEATURES", "1", "a", bad[i]);
    /* The good features before a bad one do not make the request less
     * empty. */
    REQUEST_FAILS("the value of feature 'caf\xc3\xa9' must be a finite number",
                  "TEXT", "x", "FEATURES", "3", "a", "1", "caf\xc3\xa9", "nan", "c", "3");
}

#define NAME_MSG "feature names must be 1 to 64 bytes of UTF-8 without control characters"

/* TEXT x FEATURES 1 <name> 1: GATE_OK if the name was taken, GATE_ERR if it
 * was refused as a name, something else if anything is not as it should. */
static int request_named(const char *name, size_t len) {
    gateRequest req;
    args a;
    int pos = 0, rc;

    args_set(&a, "TEXT", "x", "FEATURES", "1", NULL);
    args_add(&a, name, len);
    args_add(&a, "1", 1);
    rc = request(&a, &pos, &req);
    if (rc == GATE_OK && (pos != 6 || req.nfeatures != 1 || req.features[0].name_len != len ||
                          req.features[0].name != a.v[4].ptr))
        rc = -2;
    if (rc == GATE_ERR && (pos != 0 || !empty(&req) || strcmp(g_err, NAME_MSG) != 0)) rc = -2;
    args_free(&a);
    return rc;
}

/* gate_args.h: names follow the rule of labels. */
TEST(request_feature_names) {
    static const bytes good[] = {
        {LIT("mentioned")}, {LIT("thread depth")}, {LIT("caf\xc3\xa9")}, {LIT("1")},
        {LIT("TEXT")}
    };
    static const bytes bad[] = {
        {LIT("")}, {LIT("a\nb")}, {LIT("a\tb")}, {LIT("a\x7f")}, {LIT("a\0b")}, {LIT("ok\0")},
        {LIT("\xff")}, {LIT("caf\xc3")}, {LIT("\xc0\xaf")}, {LIT("\xc2\x85")}
    };
    char longest[80];
    gateRequest req;
    args a;
    size_t i;
    int pos = 0;

    for (i = 0; i < COUNTOF(good); i++)
        CHECK_ON(good[i].s, request_named(good[i].s, good[i].len) == GATE_OK);
    for (i = 0; i < COUNTOF(bad); i++)
        CHECK_ON(bad[i].s, request_named(bad[i].s, bad[i].len) == GATE_ERR);
    CHECK(request_named(repeat(longest, 'n', 64), 64) == GATE_OK);
    CHECK(request_named(repeat(longest, 'n', 65), 65) == GATE_ERR);

    /* A name given twice is for the featurizer to refuse, not the parser. */
    args_set(&a, "TEXT", "x", "FEATURES", "2", "a", "1", "a", "2", NULL);
    CHECK(request(&a, &pos, &req) == GATE_OK);
    CHECK(pos == 8 && req.nfeatures == 2);
    CHECK(req.features[0].value == 1.0 && req.features[1].value == 2.0);
    args_free(&a);
}

int main(void) {
    RUN(double_accepts_decimal_numbers);
    RUN(double_rejects_everything_else);
    RUN(double_length_limit);
    RUN(u64_accepts_digits);
    RUN(u64_rejects_everything_else);
    RUN(arg_is_case_insensitive_and_exact);
    RUN(hex64_known_values);
    RUN(hex64_parse_is_strict);
    RUN(hex64_round_trip);
    RUN(double_bits_known_values);
    RUN(double_bits_parse_is_strict);
    RUN(double_bits_must_be_finite);
    RUN(double_bits_round_trip);
    RUN(id_format);
    RUN(id_parse);
    RUN(id_parse_is_strict);
    RUN(id_round_trip);

    RUN(create_classifier);
    RUN(create_score);
    RUN(create_any_order);
    RUN(create_defaults);
    RUN(create_option_words_in_any_case);
    RUN(create_labels_that_spell_option_words);
    RUN(create_labels_are_exact);
    RUN(create_numbers);
    RUN(create_option_twice);
    RUN(create_unknown_option);
    RUN(create_truncated);
    RUN(create_count);
    RUN(create_alpha_is_required);
    RUN(create_classes_or_score);
    RUN(create_lists_need_classes);
    RUN(create_number_of_classes);
    RUN(create_labels);
    RUN(create_duplicate_labels);
    RUN(create_defer_every_label);
    RUN(create_unknown_label);
    RUN(create_label_twice_in_a_list);
    RUN(create_label_in_both_lists);
    RUN(create_class_alpha_value);
    RUN(create_server_values);
    RUN(create_error_buffer);
    RUN(create_out_of_memory);
    RUN(create_survives_edits);

    RUN(request_text_only);
    RUN(request_text_and_features);
    RUN(request_stops_at_the_next_argument);
    RUN(request_text_bytes);
    RUN(request_needs_text);
    RUN(request_feature_count);
    RUN(request_most_features);
    RUN(request_feature_values);
    RUN(request_feature_names);
    return TEST_REPORT();
}

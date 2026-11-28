/*
 * Gate data type: unit tests of the command argument grammar (gate_args.c).
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * How these tests are built:
 *
 * EXACT-SIZE ARGUMENTS. Redis hands a command its arguments as (pointer,
 * length) pairs. Here every argument lives in its own heap block of exactly
 * 'len' bytes with no terminator, so a parser that reads one byte past an
 * argument, or relies on a NUL being there, is caught by ASan / valgrind.
 *
 * A COUNTING, FAILING ALLOCATOR. The library allocates through hooks, and
 * the test installs its own: it counts live blocks, so every test ends with
 * a leak check that does not need valgrind, and it can fail the N-th
 * request, so every allocation failure path of a parser is walked.
 *
 * EXPECTATIONS COME FROM THE CONTRACT. Expected values are literals taken
 * from README.md / gate_args.h (defaults and limits are written as numbers,
 * not as the macros the parser uses), well known IEEE-754 bit patterns, or
 * values computed with exact rational arithmetic outside this program.
 * Messages written by the parser are compared in full; messages that belong
 * to gateConfigValidate() are matched by their distinctive part.
 */

#include <float.h>
#include "testutil.h"
#include "gate_args.h"

/* ------------------------------ Test allocator ---------------------------- */

static long g_alloc_live = 0;    /* blocks handed out and not yet released */
static long g_alloc_calls = 0;   /* requests since the counter was reset */
static long g_alloc_fail_at = 0; /* fail the request with this ordinal; 0 = never */

static int allocMustFail(void) {
    return ++g_alloc_calls == g_alloc_fail_at;
}

static void *testMalloc(size_t n) {
    void *p;

    if (allocMustFail()) return NULL;
    p = malloc(n);
    if (p != NULL) g_alloc_live++;
    return p;
}

static void *testCalloc(size_t nmemb, size_t size) {
    void *p;

    if (allocMustFail()) return NULL;
    p = calloc(nmemb, size);
    if (p != NULL) g_alloc_live++;
    return p;
}

static void *testRealloc(void *old, size_t n) {
    void *p;

    if (allocMustFail()) return NULL;
    p = realloc(old, n);
    if (p != NULL && old == NULL) g_alloc_live++;
    return p;
}

static void testFree(void *p) {
    if (p != NULL) g_alloc_live--;
    free(p);
}

/* ------------------------------ Argument vectors -------------------------- */

static void testDie(const char *why) {
    fprintf(stderr, "test harness failure: %s\n", why);
    exit(2);
}

/* Heap block of exactly 'len' bytes (see the file comment). */
static char *exactCopy(const char *s, size_t len) {
    char *copy = malloc(len);

    if (copy == NULL && len > 0) testDie("out of memory");
    if (len > 0) memcpy(copy, s, len);
    return copy;
}

typedef struct testArgs {
    gateArg *v;
    int n, cap;
} testArgs;

static void argsInit(testArgs *a) {
    a->v = NULL;
    a->n = a->cap = 0;
}

static void argsPushLen(testArgs *a, const char *s, size_t len) {
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 16;
        a->v = realloc(a->v, sizeof(gateArg) * (size_t)a->cap);
        if (a->v == NULL) testDie("out of memory");
    }
    a->v[a->n].ptr = exactCopy(s, len);
    a->v[a->n].len = len;
    a->n++;
}

static void argsPush(testArgs *a, const char *s) {
    argsPushLen(a, s, strlen(s));
}

/* A C string array as an argument vector. */
static void argsPushAll(testArgs *a, const char *const *strs, int n) {
    int i;

    for (i = 0; i < n; i++) argsPush(a, strs[i]);
}

/* A command line split on single spaces: the readable way to write most
 * cases. "a  b" yields an empty argument between a and b. Arguments that
 * need spaces or arbitrary bytes are pushed with argsPushLen(). */
static void argsSplit(testArgs *a, const char *line) {
    const char *p = line;

    while (*p != '\0') {
        const char *sp = strchr(p, ' ');
        size_t len = sp != NULL ? (size_t)(sp - p) : strlen(p);

        argsPushLen(a, p, len);
        p += len;
        if (*p == ' ') p++;
    }
}

static void argsFree(testArgs *a) {
    int i;

    for (i = 0; i < a->n; i++) free((void *)a->v[i].ptr);
    free(a->v);
    argsInit(a);
}

/* Literal with its length, for arguments that contain NUL bytes. */
#define LIT(s) (s), (sizeof(s) - 1)

/* ------------------------------ Shared helpers ---------------------------- */

#define ERRLEN 256
static char g_err[ERRLEN];

/* The buffer starts as junk so that a parser that fails without writing a
 * message is noticed. */
static void poisonError(void) {
    memset(g_err, '#', ERRLEN - 1);
    g_err[ERRLEN - 1] = '\0';
}

static int errorHas(const char *needle) {
    if (strstr(g_err, needle) != NULL) return 1;
    printf("    message was \"%s\", expected it to contain \"%s\"\n", g_err, needle);
    return 0;
}

static int isAllZero(const void *p, size_t n) {
    const unsigned char *b = p;
    size_t i;

    for (i = 0; i < n; i++)
        if (b[i] != 0) return 0;
    return 1;
}

static int sameString(const char *a, const char *b) {
    if (a == NULL || b == NULL) return a == b;
    return strcmp(a, b) == 0;
}

/* ============================== Scalar helpers ============================ */

static int parseDouble(const char *s, size_t len, double *out) {
    char *copy = exactCopy(s, len);
    int rc = gateParseDouble(copy, len, out);

    free(copy);
    return rc;
}

static int parseU64(const char *s, size_t len, uint64_t *out) {
    char *copy = exactCopy(s, len);
    int rc = gateParseU64(copy, len, out);

    free(copy);
    return rc;
}

/* Accepted, with exactly this value (the sign of zero included). */
static int doubleIs(const char *s, size_t len, double expect) {
    double v = -12345.5;

    if (parseDouble(s, len, &v) && v == expect && !signbit(v) == !signbit(expect)) return 1;
    printf("    gateParseDouble(\"%.*s\") gave %.17g, expected %.17g\n",
           (int)len, s, v, expect);
    return 0;
}

/* Rejected, with the output left alone. */
static int doubleRejected(const char *s, size_t len) {
    double v = 123.25;

    if (!parseDouble(s, len, &v) && v == 123.25) return 1;
    printf("    gateParseDouble accepted (or wrote its output for) a %d byte argument "
           "starting \"%.*s\": %.17g\n", (int)len, (int)len, s, v);
    return 0;
}

TEST(double_accepts_decimal_syntax) {
    static const struct {
        const char *s;
        double v;
    } cases[] = {
        {"0.05", 0.05},
        {".5", 0.5},
        {"1e-3", 0.001},
        {"-2", -2.0},
        {"+1", 1.0},
        {"5.", 5.0},
        {"1E5", 100000.0},
        {"0", 0.0},
        {"1", 1.0},
        {"00012.50", 12.5},
        {"+.5e+1", 5.0},
        {"-1.25e-2", -0.0125},
        {"0.1", 0.1},
        {"0.02", 0.02},
        {"1000000", 1000000.0},
        {"0.3333333333333333", 1.0 / 3.0},
        {"123456789012345678", 123456789012345680.0}, /* rounds to nearest */
        {"1.7976931348623157e308", DBL_MAX},
        {"-1.7976931348623157e308", -DBL_MAX},
        {"2.2250738585072014e-308", DBL_MIN},
        {"4.9e-324", 4.9406564584124654e-324},  /* smallest denormal */
        {"2.225073858507201e-308", 2.225073858507201e-308}, /* largest denormal */
    };
    char longest[64];
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        CHECK(doubleIs(cases[i].s, strlen(cases[i].s), cases[i].v));

    /* The sign of a zero is the client's. */
    CHECK(doubleIs(LIT("-0"), -0.0));
    CHECK(doubleIs(LIT("-0.0e0"), -0.0));
    CHECK(doubleIs(LIT("+0"), 0.0));

    /* Too small for a double is not an error: it rounds, like 0.1 does. */
    CHECK(doubleIs(LIT("1e-999"), 0.0));
    CHECK(doubleIs(LIT("-1e-999"), -0.0));

    /* 63 bytes is the longest argument: "0." + 60 zeros + "1" = 1e-61. */
    memset(longest, '0', sizeof(longest));
    longest[1] = '.';
    longest[62] = '1';
    CHECK(doubleIs(longest, 63, 1e-61));
}

TEST(double_rejects_everything_else) {
    static const char *const bad[] = {
        /* white space anywhere */
        " ", " 1", "1 ", "\t1", "1\t", "\n1", "1\n", "1\r\n", " 1 ", "1 2", "\v1", "\f1",
        /* hex floats */
        "0x10", "0X10", "0x1p3", "0x1.8p1", "-0x1", "0x",
        /* non finite spellings, any case */
        "nan", "NaN", "NAN", "-nan", "+nan", "nan(1)", "nan()", "inf", "Inf", "INF", "-inf",
        "+inf", "infinity", "Infinity", "INFINITY", "-infinity", "iNf",
        /* overflow to an infinity */
        "1e999", "-1e999", "1e309", "1.8e308", "-1.8e308",
        /* malformed arrangements of legal characters */
        "1e", "1e+", "1e-", "e5", "E5", ".", "+", "-", "+.", "-.", ".e1", "--1", "++1", "+-1",
        "-+1", "1.2.3", "1..2", "1e1.5", "1e5e5", "1-", "1+", "1-2", "e", "+e1", "1e+-2",
        /* other number-like things */
        "1,5", "1_000", "1f", "1.0f", "1d", "1L", "0b1", "$1", "1%", "1/3", "one", "abc",
        "0.5x", "x0.5", "(1)", "1'000", "#1",
        /* digits that are not ASCII */
        "\xd9\xa1" /* ARABIC-INDIC DIGIT ONE */, "\xef\xbc\x91" /* FULLWIDTH DIGIT ONE */,
        "1\xc2\xa0" /* 1 + NO-BREAK SPACE */,
    };
    char longest[65];
    double v;
    size_t i;

    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(doubleRejected(bad[i], strlen(bad[i])));

    /* The empty argument, with and without a pointer. */
    CHECK(doubleRejected("", 0));
    v = 123.25;
    CHECK(gateParseDouble(NULL, 0, &v) == 0 && v == 123.25);

    /* Embedded NULs: strtod() would stop there and report a clean parse. */
    CHECK(doubleRejected(LIT("1\0")));
    CHECK(doubleRejected(LIT("\0" "1")));
    CHECK(doubleRejected(LIT("1\0" "5")));
    CHECK(doubleRejected(LIT("0.5\0junk")));
    CHECK(doubleRejected(LIT("\0")));

    /* 64 bytes: one more than the limit, although the value is fine. */
    memset(longest, '0', sizeof(longest));
    longest[1] = '.';
    longest[63] = '1';
    CHECK(doubleRejected(longest, 64));
    memset(longest, '1', sizeof(longest));
    /* The same digits fit in 63 bytes. */
    CHECK(doubleIs(longest, 63,
                   111111111111111111111111111111111111111111111111111111111111111.0));
    CHECK(doubleRejected(longest, 64));
    CHECK(doubleRejected(longest, 65));
}

TEST(double_reads_exactly_len_bytes) {
    /* Bytes after the argument must not leak into the value: these would
     * read 0.5123, 12000 and 100000 if 'len' were ignored. */
    static const char a[] = "0.5123", b[] = "12e3", c[] = "1e5";
    double v = 0;

    CHECK(gateParseDouble(a, 3, &v) && v == 0.5);
    CHECK(gateParseDouble(b, 2, &v) && v == 12.0);
    CHECK(gateParseDouble(c, 1, &v) && v == 1.0);
    v = 7.0;
    CHECK(!gateParseDouble(c, 2, &v) && v == 7.0); /* "1e" */
}

static int u64Is(const char *s, size_t len, uint64_t expect) {
    uint64_t v = 0xdeadbeefull;

    if (parseU64(s, len, &v) && v == expect) return 1;
    printf("    gateParseU64(\"%.*s\") gave %llu, expected %llu\n", (int)len, s,
           (unsigned long long)v, (unsigned long long)expect);
    return 0;
}

static int u64Rejected(const char *s, size_t len) {
    uint64_t v = 0xdeadbeefull;

    if (!parseU64(s, len, &v) && v == 0xdeadbeefull) return 1;
    printf("    gateParseU64 accepted (or wrote its output for) \"%.*s\": %llu\n",
           (int)len, s, (unsigned long long)v);
    return 0;
}

TEST(u64_accepts_digits_only) {
    CHECK(u64Is(LIT("0"), 0));
    CHECK(u64Is(LIT("7"), 7));
    CHECK(u64Is(LIT("42"), 42));
    CHECK(u64Is(LIT("007"), 7));
    CHECK(u64Is(LIT("4294967295"), 4294967295ull));
    CHECK(u64Is(LIT("4294967296"), 4294967296ull));
    CHECK(u64Is(LIT("9223372036854775807"), 9223372036854775807ull));
    CHECK(u64Is(LIT("9223372036854775808"), 9223372036854775808ull));
    CHECK(u64Is(LIT("18446744073709551609"), 18446744073709551609ull));
    CHECK(u64Is(LIT("18446744073709551615"), 18446744073709551615ull)); /* 2^64 - 1 */
    /* Zero padding is fine within the 20 digit width. */
    CHECK(u64Is(LIT("00000000000000000000"), 0));
    CHECK(u64Is(LIT("00000000000000000001"), 1));
    CHECK(u64Is(LIT("09999999999999999999"), 9999999999999999999ull));
}

TEST(u64_rejects_everything_else) {
    static const char *const bad[] = {
        /* one past the top in the last digit, in an earlier digit, and far past */
        "18446744073709551616", "18446744073709551620", "18446744073709551625",
        "18446744073709552000", "20000000000000000000", "99999999999999999999",
        /* 21 digits, whatever the value */
        "000000000000000000000", "000000000000000000001", "184467440737095516150",
        /* signs, space, other syntaxes */
        "+1", "-1", "-0", "+0", " 1", "1 ", "\t1", "1\n", "1.0", "1.", ".1", "1e3", "0x10",
        "12a", "a12", "1,000", "1_000", "0b1", "abc", "\xd9\xa1", "\xef\xbc\x91",
    };
    static const char seq[] = "123456";
    uint64_t v = 5;
    size_t i;

    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(u64Rejected(bad[i], strlen(bad[i])));
    CHECK(u64Rejected("", 0));
    CHECK(gateParseU64(NULL, 0, &v) == 0 && v == 5);
    CHECK(u64Rejected(LIT("1\0")));
    CHECK(u64Rejected(LIT("\0" "1")));
    CHECK(u64Rejected(LIT("12\0" "3")));

    /* Only 'len' bytes are read. */
    CHECK(gateParseU64(seq, 3, &v) && v == 123);
    CHECK(gateParseU64(seq, 1, &v) && v == 1);
}

TEST(arg_is_folds_ascii_letters_only) {
    static const gateArg lower = {LIT("mode")}, upper = {LIT("MODE")}, mixed = {LIT("MoDe")};
    static const gateArg shorter = {LIT("MOD")}, longer = {LIT("MODES")};
    static const gateArg nul = {LIT("MODE\0")}, empty = {"", 0}, null = {NULL, 0};
    static const gateArg l2 = {LIT("l2")}, accent = {LIT("MOD\xc3\x89")};
    /* 0x60 and 0x7b are 0x40 and 0x5b plus the "lower case" bit, but they
     * are not letters: a blanket bit mask would make these match. */
    static const gateArg backtick = {LIT("a`")}, brace = {LIT("a{")};
    static const gateArg at = {LIT("A@")}, bracket = {LIT("A[")};

    CHECK(gateArgIs(&lower, "MODE"));
    CHECK(gateArgIs(&upper, "MODE"));
    CHECK(gateArgIs(&mixed, "MODE"));
    CHECK(!gateArgIs(&shorter, "MODE"));
    CHECK(!gateArgIs(&longer, "MODE"));
    CHECK(!gateArgIs(&nul, "MODE"));
    CHECK(!gateArgIs(&empty, "MODE"));
    CHECK(!gateArgIs(&null, "MODE"));
    CHECK(!gateArgIs(&upper, "MODEL"));
    CHECK(!gateArgIs(&upper, "NODE"));
    CHECK(gateArgIs(&l2, "L2"));
    CHECK(!gateArgIs(&l2, "L3"));
    CHECK(!gateArgIs(&accent, "MODE"));
    CHECK(gateArgIs(&at, "A@"));
    CHECK(!gateArgIs(&backtick, "A@"));
    CHECK(gateArgIs(&bracket, "A["));
    CHECK(!gateArgIs(&brace, "A["));
}

/* ----------------------------- Observation ids ---------------------------- */

static int parseObservation(const char *s, size_t len, uint64_t *epoch, uint64_t *seq) {
    char *copy = exactCopy(s, len);
    int rc = gateParseObservationId(copy, len, epoch, seq);

    free(copy);
    return rc;
}

static int observationIs(const char *s, size_t len, uint64_t epoch, uint64_t seq) {
    uint64_t e = 1, q = 2;

    if (parseObservation(s, len, &e, &q) && e == epoch && q == seq) return 1;
    printf("    gateParseObservationId(\"%.*s\") gave %016llx-%llu\n", (int)len, s,
           (unsigned long long)e, (unsigned long long)q);
    return 0;
}

static int observationRejected(const char *s, size_t len) {
    uint64_t e = 0x1111, q = 0x2222;

    if (!parseObservation(s, len, &e, &q) && e == 0x1111 && q == 0x2222) return 1;
    printf("    gateParseObservationId accepted (or wrote its outputs for) \"%.*s\"\n",
           (int)len, s);
    return 0;
}

TEST(observation_id_format) {
    char buf[GATE_OBSERVATION_ID_MAX], small[64];

    /* The example of gate_args.h. */
    CHECK_EQ_INT(gateFormatObservationId(buf, sizeof(buf), 0x5d0c3f9a7b21e684ull, 1042), 21);
    CHECK_EQ_STR(buf, "5d0c3f9a7b21e684-1042");

    CHECK_EQ_INT(gateFormatObservationId(buf, sizeof(buf), 0, 1), 18);
    CHECK_EQ_STR(buf, "0000000000000000-1");
    CHECK_EQ_INT(gateFormatObservationId(buf, sizeof(buf), 0xabcdefull, 10), 19);
    CHECK_EQ_STR(buf, "0000000000abcdef-10");
    /* The longest id fits GATE_OBSERVATION_ID_MAX. */
    CHECK_EQ_INT(gateFormatObservationId(buf, sizeof(buf), UINT64_MAX, UINT64_MAX), 37);
    CHECK_EQ_STR(buf, "ffffffffffffffff-18446744073709551615");

    /* A buffer with room for the id and its terminator, and not a byte more. */
    memset(small, 'z', sizeof(small));
    CHECK_EQ_INT(gateFormatObservationId(small, 22, 0x5d0c3f9a7b21e684ull, 1042), 21);
    CHECK_EQ_STR(small, "5d0c3f9a7b21e684-1042");
    CHECK(small[22] == 'z');

    /* One byte short: nothing but an empty string is written. */
    memset(small, 'z', sizeof(small));
    CHECK_EQ_INT(gateFormatObservationId(small, 21, 0x5d0c3f9a7b21e684ull, 1042), 0);
    CHECK(small[0] == '\0' && small[1] == 'z' && small[21] == 'z');
    memset(small, 'z', sizeof(small));
    CHECK_EQ_INT(gateFormatObservationId(small, 0, 1, 1), 0);
    CHECK(small[0] == 'z');
    CHECK_EQ_INT(gateFormatObservationId(NULL, 0, 1, 1), 0);
}

TEST(observation_id_round_trip) {
    uint64_t rng = 0x6a09e667f3bcc908ull;
    int i;

    for (i = 0; i < 2000; i++) {
        char buf[GATE_OBSERVATION_ID_MAX], expect[64];
        uint64_t epoch = gate_test_rand64(&rng);
        /* Every decimal length from 1 to 20 digits shows up. */
        uint64_t seq = gate_test_rand64(&rng) >> (gate_test_rand64(&rng) % 64);
        uint64_t e = 0, q = 0;
        size_t n;

        if (seq == 0) seq = 1;
        n = gateFormatObservationId(buf, sizeof(buf), epoch, seq);
        /* printf() is an implementation of the format independent of ours. */
        snprintf(expect, sizeof(expect), "%016llx-%llu", (unsigned long long)epoch,
                 (unsigned long long)seq);
        CHECK_EQ_STR(buf, expect);
        CHECK_EQ_INT(n, strlen(expect));
        CHECK(parseObservation(buf, n, &e, &q) && e == epoch && q == seq);
    }
}

TEST(observation_id_parse_is_strict) {
    /* Accepted spellings. */
    CHECK(observationIs(LIT("5d0c3f9a7b21e684-1042"), 0x5d0c3f9a7b21e684ull, 1042));
    CHECK(observationIs(LIT("5D0C3F9A7B21E684-1042"), 0x5d0c3f9a7b21e684ull, 1042));
    CHECK(observationIs(LIT("5d0C3f9A7b21E684-1042"), 0x5d0c3f9a7b21e684ull, 1042));
    CHECK(observationIs(LIT("0000000000000000-1"), 0, 1));
    CHECK(observationIs(LIT("ffffffffffffffff-18446744073709551615"), UINT64_MAX, UINT64_MAX));
    CHECK(observationIs(LIT("0123456789abcdef-10"), 0x0123456789abcdefull, 10));

    /* 15 and 17 hex digits. */
    CHECK(observationRejected(LIT("5d0c3f9a7b21e68-1042")));
    CHECK(observationRejected(LIT("5d0c3f9a7b21e6840-1042")));
    /* Not hex. */
    CHECK(observationRejected(LIT("5d0c3f9a7b21e68g-1042")));
    CHECK(observationRejected(LIT("5d0c3f9a7b21e68 -1042")));
    CHECK(observationRejected(LIT("0x0c3f9a7b21e684-1042")));
    CHECK(observationRejected(LIT("+d0c3f9a7b21e684-1042")));
    /* Leading zeros in the sequence, and sequence 0. */
    CHECK(observationRejected(LIT("5d0c3f9a7b21e684-01042")));
    CHECK(observationRejected(LIT("5d0c3f9a7b21e684-01")));
    CHECK(observationRejected(LIT("5d0c3f9a7b21e684-0")));
    CHECK(observationRejected(LIT("5d0c3f9a7b21e684-00")));
    /* Sequence overflow: 2^64, a larger 20 digit number, 21 digits. */
    CHECK(observationRejected(LIT("5d0c3f9a7b21e684-18446744073709551616")));
    CHECK(observationRejected(LIT("5d0c3f9a7b21e684-99999999999999999999")));
    CHECK(observationRejected(LIT("5d0c3f9a7b21e684-100000000000000000000")));
    /* Missing or wrong separator, missing parts. */
    CHECK(observationRejected(LIT("5d0c3f9a7b21e6841042")));
    CHECK(observationRejected(LIT("5d0c3f9a7b21e684_1042")));
    CHECK(observationRejected(LIT("5d0c3f9a7b21e684 1042")));
    CHECK(observationRejected(LIT("5d0c3f9a7b21e684--1042")));
    CHECK(observationRejected(LIT("5d0c3f9a7b21e684")));
    CHECK(observationRejected(LIT("5d0c3f9a7b21e684-")));
    CHECK(observationRejected(LIT("-1042")));
    CHECK(observationRejected(LIT("1042")));
    CHECK(observationRejected("", 0));
    /* Signs and other syntaxes in the sequence. */
    CHECK(observationRejected(LIT("5d0c3f9a7b21e684-+1")));
    CHECK(observationRejected(LIT("5d0c3f9a7b21e684-1e3")));
    CHECK(observationRejected(LIT("5d0c3f9a7b21e684-0x1")));
    CHECK(observationRejected(LIT("5d0c3f9a7b21e684-1.0")));
    /* Garbage around a valid id. */
    CHECK(observationRejected(LIT("5d0c3f9a7b21e684-1042 ")));
    CHECK(observationRejected(LIT("5d0c3f9a7b21e684-1042x")));
    CHECK(observationRejected(LIT("5d0c3f9a7b21e684-1042\0")));
    CHECK(observationRejected(LIT("5d0c3f9a7b21e684-1042\n")));
    CHECK(observationRejected(LIT("5d0c3f9a7b21e684-1042-1")));
    CHECK(observationRejected(LIT(" 5d0c3f9a7b21e684-1042")));
    CHECK(observationRejected(LIT("\"5d0c3f9a7b21e684-1042\"")));
}

/* ------------------------- Hex words and double bits ---------------------- */

static int parseHex(const char *s, size_t len, uint64_t *out) {
    char *copy = exactCopy(s, len);
    int rc = gateParseHex64(copy, len, out);

    free(copy);
    return rc;
}

static int parseBits(const char *s, size_t len, double *out) {
    char *copy = exactCopy(s, len);
    int rc = gateParseDoubleBits(copy, len, out);

    free(copy);
    return rc;
}

static int hexRejected(const char *s, size_t len) {
    uint64_t v = 0x1234;

    if (!parseHex(s, len, &v) && v == 0x1234) return 1;
    printf("    gateParseHex64 accepted (or wrote its output for) \"%.*s\"\n", (int)len, s);
    return 0;
}

static int bitsRejected(const char *s, size_t len) {
    double v = 123.25;

    if (!parseBits(s, len, &v) && v == 123.25) return 1;
    printf("    gateParseDoubleBits accepted (or wrote its output for) \"%.*s\"\n",
           (int)len, s);
    return 0;
}

TEST(hex64_format_and_parse) {
    uint64_t rng = 0xbb67ae8584caa73bull, v = 0;
    char buf[32];
    int i;

    memset(buf, 'z', sizeof(buf));
    gateFormatHex64(buf, 0);
    CHECK_EQ_STR(buf, "0000000000000000");
    CHECK(buf[17] == 'z'); /* 16 digits and a terminator, nothing more */
    gateFormatHex64(buf, 0x0123456789abcdefull);
    CHECK_EQ_STR(buf, "0123456789abcdef");
    gateFormatHex64(buf, UINT64_MAX);
    CHECK_EQ_STR(buf, "ffffffffffffffff");
    gateFormatHex64(buf, 0x8000000000000001ull);
    CHECK_EQ_STR(buf, "8000000000000001");

    CHECK(parseHex(LIT("0123456789abcdef"), &v) && v == 0x0123456789abcdefull);
    CHECK(parseHex(LIT("0123456789ABCDEF"), &v) && v == 0x0123456789abcdefull);
    CHECK(parseHex(LIT("FfFfFfFfFfFfFfFf"), &v) && v == UINT64_MAX);
    CHECK(parseHex(LIT("0000000000000000"), &v) && v == 0);

    for (i = 0; i < 2000; i++) {
        uint64_t x = gate_test_rand64(&rng), back = ~x;
        char expect[32];

        gateFormatHex64(buf, x);
        snprintf(expect, sizeof(expect), "%016llx", (unsigned long long)x);
        CHECK_EQ_STR(buf, expect);
        CHECK(parseHex(buf, 16, &back) && back == x);
    }

    CHECK(hexRejected(LIT("0123456789abcde")));   /* 15 */
    CHECK(hexRejected(LIT("0123456789abcdef0"))); /* 17 */
    CHECK(hexRejected("", 0));
    CHECK(hexRejected(LIT("0123456789abcdeg")));
    CHECK(hexRejected(LIT("0x23456789abcdef")));
    CHECK(hexRejected(LIT(" 123456789abcdef")));
    CHECK(hexRejected(LIT("123456789abcdef ")));
    CHECK(hexRejected(LIT("+123456789abcdef")));
    CHECK(hexRejected(LIT("-123456789abcdef")));
    CHECK(hexRejected(LIT("0123456789abcde\0")));
    CHECK(hexRejected(LIT("01234567-9abcdef")));
    v = 9;
    CHECK(gateParseHex64(NULL, 0, &v) == 0 && v == 9);
}

TEST(double_bits_known_patterns) {
    /* IEEE-754 binary64 patterns, from the standard and not from the code. */
    static const struct {
        double v;
        const char *hex;
    } cases[] = {
        {0.0, "0000000000000000"},
        {-0.0, "8000000000000000"},
        {1.0, "3ff0000000000000"},
        {-2.0, "c000000000000000"},
        {1.0 / 3.0, "3fd5555555555555"},
        {0.1, "3fb999999999999a"},
        {0.02, "3f947ae147ae147b"},
        {3.141592653589793, "400921fb54442d18"},
        {DBL_MAX, "7fefffffffffffff"},
        {-DBL_MAX, "ffefffffffffffff"},
        {DBL_MIN, "0010000000000000"},
        {4.9406564584124654e-324, "0000000000000001"}, /* smallest denormal */
        {2.225073858507201e-308, "000fffffffffffff"},  /* largest denormal */
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char buf[32];
        double back = 99.0;

        memset(buf, 'z', sizeof(buf));
        gateFormatDoubleBits(buf, cases[i].v);
        CHECK_EQ_STR(buf, cases[i].hex);
        CHECK(buf[17] == 'z');
        CHECK(parseBits(cases[i].hex, 16, &back));
        /* Bit for bit: == would call 0.0 and -0.0 equal. */
        CHECK(memcmp(&back, &cases[i].v, sizeof(double)) == 0);
    }
}

TEST(double_bits_round_trip_and_strictness) {
    uint64_t rng = 0x3c6ef372fe94f82bull;
    double v = 0;
    int i, finite = 0;

    for (i = 0; i < 4000; i++) {
        uint64_t bits = gate_test_rand64(&rng);
        char hex[32], again[32];
        double d, back = 0;
        int is_finite = ((bits >> 52) & 0x7ff) != 0x7ff; /* exponent not all ones */

        memcpy(&d, &bits, sizeof(d));
        gateFormatHex64(hex, bits);
        gateFormatDoubleBits(again, d);
        CHECK_EQ_STR(again, hex);
        if (is_finite) {
            finite++;
            CHECK(parseBits(hex, 16, &back) && memcmp(&back, &d, sizeof(d)) == 0);
        } else {
            CHECK(bitsRejected(hex, 16));
        }
    }
    CHECK(finite > 3900); /* the loop did exercise the accepting path */

    /* Upper case digits name the same bits. */
    CHECK(parseBits(LIT("3FD5555555555555"), &v) && v == 1.0 / 3.0);

    CHECK(bitsRejected(LIT("3fd555555555555")));   /* 15 */
    CHECK(bitsRejected(LIT("3fd55555555555555"))); /* 17 */
    CHECK(bitsRejected("", 0));
    CHECK(bitsRejected(LIT("3fd555555555555g")));
    CHECK(bitsRejected(LIT("0x3fd55555555555")));
    CHECK(bitsRejected(LIT("0.33333333333333")));
    CHECK(bitsRejected(LIT("3fd5555555555555\0")));

    /* Nothing non-finite comes out of this layer, not even as bits. */
    CHECK(bitsRejected(LIT("7ff0000000000000"))); /* +inf */
    CHECK(bitsRejected(LIT("fff0000000000000"))); /* -inf */
    CHECK(bitsRejected(LIT("7ff8000000000000"))); /* quiet NaN */
    CHECK(bitsRejected(LIT("7ff0000000000001"))); /* signalling NaN */
    CHECK(bitsRejected(LIT("ffffffffffffffff"))); /* NaN, all ones */
}

/* ================================= GCREATE ================================ */

static int configIsZero(const gateConfig *cfg) {
    return isAllZero(cfg, sizeof(*cfg));
}

/* Parse an argument vector. The config starts as junk: the parser must
 * overwrite it, and zero it when it fails. */
static int createArgs(const testArgs *a, gateConfig *cfg) {
    memset(cfg, 0xa5, sizeof(*cfg));
    poisonError();
    return gateParseCreate(a->v, a->n, cfg, g_err, sizeof(g_err));
}

static int createLine(const char *line, gateConfig *cfg) {
    testArgs a;
    int rc;

    argsInit(&a);
    argsSplit(&a, line);
    rc = createArgs(&a, cfg);
    /* The arguments are released before the caller looks at the config: a
     * config that borrowed from them would be a use after free. */
    argsFree(&a);
    return rc;
}

/* 1 if the command is accepted. On refusal the message is shown and the
 * test should stop: a refused config holds nothing to inspect. */
static int createAccepted(const char *line, gateConfig *cfg) {
    if (createLine(line, cfg) == GATE_OK) return 1;
    printf("    \"%s\" was refused: %s\n", line, g_err);
    memset(cfg, 0, sizeof(*cfg));
    return 0;
}

#define CREATE_OK_OR_RETURN(line, cfg) do { \
    int ok_ = createAccepted((line), (cfg)); \
    CHECK(ok_); \
    if (!ok_) return; \
} while (0)

/* 1 if the vector is refused; '*zeroed' tells whether the config came back
 * zeroed. A config produced by mistake is released. */
static int createArgsRefused(const testArgs *a, int *zeroed) {
    gateConfig cfg;
    int rc = createArgs(a, &cfg);

    *zeroed = configIsZero(&cfg);
    if (rc == GATE_OK) gateConfigFree(&cfg);
    return rc == GATE_ERR;
}

static int createRefused(const char *line, int *zeroed) {
    testArgs a;
    int refused;

    argsInit(&a);
    argsSplit(&a, line);
    refused = createArgsRefused(&a, zeroed);
    argsFree(&a);
    if (!refused) printf("    \"%s\" was accepted\n", line);
    return refused;
}

/* Refused with exactly this message, leaving a zeroed config behind. */
#define CREATE_FAILS(line, msg) do { \
    int zeroed_ = 0; \
    CHECK(createRefused((line), &zeroed_)); \
    CHECK(zeroed_); \
    CHECK_EQ_STR(g_err, (msg)); \
} while (0)

/* Same, for messages owned by gateConfigValidate(): matched by content. */
#define CREATE_FAILS_WITH(line, needle) do { \
    int zeroed_ = 0; \
    CHECK(createRefused((line), &zeroed_)); \
    CHECK(zeroed_); \
    CHECK(errorHas(needle)); \
} while (0)

#define CREATE_ARGS_FAIL(args, msg) do { \
    int zeroed_ = 0; \
    CHECK(createArgsRefused((args), &zeroed_)); \
    CHECK(zeroed_); \
    CHECK_EQ_STR(g_err, (msg)); \
} while (0)

static int labelIs(const gateConfig *cfg, uint32_t i, const char *expect) {
    if (cfg->labels != NULL && i < cfg->nclasses && sameString(cfg->labels[i], expect))
        return 1;
    printf("    label %u is not \"%s\"\n", i, expect);
    return 0;
}

/* Every field that GCREATE does not mention has its documented default.
 * The numbers are the ones printed in README.md, on purpose not the
 * GATE_DEFAULT_* macros the parser itself uses. */
static void checkUntouchedDefaults(const gateConfig *cfg) {
    CHECK(cfg->delta == 0.01);
    CHECK(cfg->audit == 0.05);
    CHECK(cfg->cold_audit == 0.05);
    CHECK_EQ_INT(cfg->nthresholds, 32);
    CHECK(cfg->lr == 0.1);
    CHECK(cfg->l2 == 0.0);
    CHECK_EQ_INT(cfg->max_memory, 64ll * 1024 * 1024);
    CHECK_EQ_INT(cfg->max_pending, 4096);
    CHECK_EQ_INT(cfg->observation_ttl_ms, 300000);
    CHECK_EQ_INT(cfg->cert_ttl_ms, 30ll * 24 * 3600 * 1000);
    CHECK_EQ_INT(cfg->class_mode, 0);
    CHECK(cfg->class_alpha == NULL);
    CHECK(cfg->class_defer == NULL);
    CHECK(cfg->drift_margin == -1.0);
    CHECK_EQ_INT(cfg->on_missing, GATE_ONMISSING_REVOKE);
    CHECK_EQ_INT(cfg->expand, 0);
}

TEST(create_spec_example_classifier) {
    /* Section 4 of the specification, as a C string array. */
    static const char *const argv[] = {
        "MODE", "CLASSIFIER", "CLASSES", "2", "ignore", "respond", "INPUTS", "2", "TEXT",
        "FEATURES", "TEXTDIM", "8192", "FEATUREDIM", "256", "ALPHA", "0.02", "DELTA", "0.01",
        "TEACHER", "router-v1", "SCHEMAVERSION", "chat-v1",
    };
    gateConfig cfg;
    testArgs a;
    char err[64];
    int rc;

    argsInit(&a);
    argsPushAll(&a, argv, (int)(sizeof(argv) / sizeof(argv[0])));
    rc = createArgs(&a, &cfg);
    argsFree(&a);
    CHECK_EQ_INT(rc, GATE_OK);
    if (rc != GATE_OK) return;

    CHECK_EQ_INT(cfg.mode, GATE_MODE_CLASSIFIER);
    CHECK(cfg.alpha == 0.02);
    CHECK_EQ_INT(cfg.nclasses, 2);
    CHECK(labelIs(&cfg, 0, "ignore"));
    CHECK(labelIs(&cfg, 1, "respond"));
    CHECK_EQ_INT(cfg.schema.inputs, GATE_INPUT_TEXT | GATE_INPUT_FEATURES);
    CHECK_EQ_INT(cfg.schema.text_dim, 8192);
    CHECK_EQ_INT(cfg.schema.vector_dim, 0);
    CHECK_EQ_INT(cfg.schema.feature_dim, 256);
    CHECK_EQ_STR(cfg.teacher, "router-v1");
    CHECK_EQ_STR(cfg.schema_version, "chat-v1");
    checkUntouchedDefaults(&cfg);
    /* "The result has passed gateConfigValidate()". */
    CHECK_EQ_INT(gateConfigValidate(&cfg, err, sizeof(err)), GATE_OK);
    gateConfigFree(&cfg);
}

TEST(create_spec_example_score) {
    gateConfig cfg;
    char err[64];

    CREATE_OK_OR_RETURN("MODE SCORE ALPHA 0.01 TEACHER equivalence-v1 SCHEMAVERSION cosine-v1",
                        &cfg);
    CHECK_EQ_INT(cfg.mode, GATE_MODE_SCORE);
    CHECK(cfg.alpha == 0.01);
    CHECK_EQ_INT(cfg.nclasses, 0);
    CHECK(cfg.labels == NULL);
    CHECK_EQ_INT(cfg.schema.inputs, 0);
    CHECK_EQ_INT(cfg.schema.text_dim, 0);
    CHECK_EQ_INT(cfg.schema.vector_dim, 0);
    CHECK_EQ_INT(cfg.schema.feature_dim, 0);
    CHECK_EQ_STR(cfg.teacher, "equivalence-v1");
    CHECK_EQ_STR(cfg.schema_version, "cosine-v1");
    checkUntouchedDefaults(&cfg);
    CHECK_EQ_INT(gateConfigValidate(&cfg, err, sizeof(err)), GATE_OK);
    gateConfigFree(&cfg);
}

TEST(create_readme_quick_start) {
    gateConfig cfg;

    CREATE_OK_OR_RETURN("MODE SCORE ALPHA 0.05 TEACHER equivalence-v1 SCHEMAVERSION cosine-v1",
                        &cfg);
    CHECK_EQ_INT(cfg.mode, GATE_MODE_SCORE);
    CHECK(cfg.alpha == 0.05);
    gateConfigFree(&cfg);

    /* No TEXTDIM / FEATUREDIM here: the configured inputs get the defaults. */
    CREATE_OK_OR_RETURN("MODE CLASSIFIER CLASSES 2 ignore respond INPUTS 2 TEXT FEATURES "
                        "ALPHA 0.02 TEACHER router-v1 SCHEMAVERSION chat-v1", &cfg);
    CHECK_EQ_INT(cfg.mode, GATE_MODE_CLASSIFIER);
    CHECK(cfg.alpha == 0.02);
    CHECK(labelIs(&cfg, 0, "ignore"));
    CHECK(labelIs(&cfg, 1, "respond"));
    CHECK_EQ_INT(cfg.schema.inputs, GATE_INPUT_TEXT | GATE_INPUT_FEATURES);
    CHECK_EQ_INT(cfg.schema.text_dim, 8192);
    CHECK_EQ_INT(cfg.schema.vector_dim, 0);
    CHECK_EQ_INT(cfg.schema.feature_dim, 256);
    checkUntouchedDefaults(&cfg);
    gateConfigFree(&cfg);
}

TEST(create_minimal_commands_get_every_default) {
    gateConfig cfg;

    CREATE_OK_OR_RETURN("MODE SCORE ALPHA 0.1", &cfg);
    CHECK_EQ_INT(cfg.mode, GATE_MODE_SCORE);
    CHECK(cfg.alpha == 0.1);
    CHECK_EQ_INT(cfg.nclasses, 0);
    CHECK(cfg.labels == NULL);
    CHECK(isAllZero(&cfg.schema, sizeof(cfg.schema)));
    CHECK_EQ_STR(cfg.teacher, "dev");
    CHECK_EQ_STR(cfg.schema_version, "dev");
    checkUntouchedDefaults(&cfg);
    gateConfigFree(&cfg);

    CREATE_OK_OR_RETURN("MODE CLASSIFIER ALPHA 0.1 CLASSES 2 a b INPUTS 1 TEXT", &cfg);
    CHECK_EQ_INT(cfg.mode, GATE_MODE_CLASSIFIER);
    CHECK_EQ_INT(cfg.nclasses, 2);
    CHECK_EQ_INT(cfg.schema.inputs, GATE_INPUT_TEXT);
    CHECK_EQ_INT(cfg.schema.text_dim, 8192);
    CHECK_EQ_INT(cfg.schema.vector_dim, 0);
    CHECK_EQ_INT(cfg.schema.feature_dim, 0);
    CHECK_EQ_STR(cfg.teacher, "dev");
    CHECK_EQ_STR(cfg.schema_version, "dev");
    checkUntouchedDefaults(&cfg);
    gateConfigFree(&cfg);
}

/* One command that sets every option to a value that is not its default,
 * as the list of its option groups. */
static const char *const g_full_groups[] = {
    "MODE CLASSIFIER", "ALPHA 0.2", "DELTA 0.05", "AUDIT 0.5", "COLDAUDIT 1",
    "THRESHOLDS 64", "CLASSES 3 a b c", "CLASSALPHA 2 a 0.1 c 0.15", "ALWAYSDEFER 1 b",
    "INPUTS 3 FEATURES VECTOR TEXT", "TEXTDIM 1024", "VECTORDIM 8", "FEATUREDIM 32",
    "LR 0.5", "L2 0.001", "TEACHER t1", "SCHEMAVERSION s1", "MAXMEMORY 1000000",
    "MAXPENDING 10", "OBSERVATIONTTL 1000", "CERTTTL 60000", "DRIFTMARGIN 0.25",
    "ONMISSING ERROR", "EXPAND YES",
};
#define FULL_GROUPS ((int)(sizeof(g_full_groups) / sizeof(g_full_groups[0])))

/* Parse the groups in the order given by 'order'. */
static int createGroups(const int *order, gateConfig *cfg) {
    testArgs a;
    int i, rc;

    argsInit(&a);
    for (i = 0; i < FULL_GROUPS; i++) argsSplit(&a, g_full_groups[order[i]]);
    rc = createArgs(&a, cfg);
    argsFree(&a);
    return rc;
}

TEST(create_every_option_is_stored) {
    gateConfig cfg;
    int order[FULL_GROUPS], i;

    CHECK_EQ_INT(FULL_GROUPS, 24); /* the README lists 24 options */
    for (i = 0; i < FULL_GROUPS; i++) order[i] = i;
    CHECK_EQ_INT(createGroups(order, &cfg), GATE_OK);
    if (configIsZero(&cfg)) {
        printf("    refused: %s\n", g_err);
        return;
    }

    CHECK_EQ_INT(cfg.mode, GATE_MODE_CLASSIFIER);
    CHECK(cfg.alpha == 0.2);
    CHECK(cfg.delta == 0.05);
    CHECK(cfg.audit == 0.5);
    CHECK(cfg.cold_audit == 1.0);
    CHECK_EQ_INT(cfg.nthresholds, 64);
    CHECK_EQ_INT(cfg.nclasses, 3);
    CHECK(labelIs(&cfg, 0, "a"));
    CHECK(labelIs(&cfg, 1, "b"));
    CHECK(labelIs(&cfg, 2, "c"));
    CHECK_EQ_INT(cfg.schema.inputs, GATE_INPUT_TEXT | GATE_INPUT_VECTOR | GATE_INPUT_FEATURES);
    CHECK_EQ_INT(cfg.schema.text_dim, 1024);
    CHECK_EQ_INT(cfg.schema.vector_dim, 8);
    CHECK_EQ_INT(cfg.schema.feature_dim, 32);
    CHECK(cfg.lr == 0.5);
    CHECK(cfg.l2 == 0.001);
    CHECK_EQ_STR(cfg.teacher, "t1");
    CHECK_EQ_STR(cfg.schema_version, "s1");
    CHECK_EQ_INT(cfg.max_memory, 1000000);
    CHECK_EQ_INT(cfg.max_pending, 10);
    CHECK_EQ_INT(cfg.observation_ttl_ms, 1000);
    CHECK_EQ_INT(cfg.cert_ttl_ms, 60000);
    CHECK(cfg.drift_margin == 0.25);
    CHECK_EQ_INT(cfg.on_missing, GATE_ONMISSING_ERROR);
    CHECK_EQ_INT(cfg.expand, 1);
    CHECK_EQ_INT(cfg.class_mode, 1);
    CHECK(cfg.class_alpha != NULL && cfg.class_defer != NULL);
    if (cfg.class_alpha != NULL && cfg.class_defer != NULL) {
        CHECK(cfg.class_alpha[0] == 0.1);
        CHECK(cfg.class_alpha[1] == 0.2); /* not listed: the gate's ALPHA */
        CHECK(cfg.class_alpha[2] == 0.15);
        CHECK_EQ_INT(cfg.class_defer[0], 0);
        CHECK_EQ_INT(cfg.class_defer[1], 1);
        CHECK_EQ_INT(cfg.class_defer[2], 0);
    }
    gateConfigFree(&cfg);
}

static int configsEqual(const gateConfig *a, const gateConfig *b) {
    uint32_t i;

    if (a->mode != b->mode || a->alpha != b->alpha || a->delta != b->delta ||
        a->audit != b->audit || a->cold_audit != b->cold_audit ||
        a->nthresholds != b->nthresholds || a->nclasses != b->nclasses)
        return 0;
    if (a->schema.inputs != b->schema.inputs || a->schema.text_dim != b->schema.text_dim ||
        a->schema.vector_dim != b->schema.vector_dim ||
        a->schema.feature_dim != b->schema.feature_dim)
        return 0;
    if (a->lr != b->lr || a->l2 != b->l2 || a->max_memory != b->max_memory ||
        a->max_pending != b->max_pending || a->observation_ttl_ms != b->observation_ttl_ms ||
        a->cert_ttl_ms != b->cert_ttl_ms)
        return 0;
    if (!sameString(a->teacher, b->teacher) || !sameString(a->schema_version, b->schema_version))
        return 0;
    if (a->class_mode != b->class_mode || a->drift_margin != b->drift_margin ||
        a->on_missing != b->on_missing || a->expand != b->expand)
        return 0;
    if ((a->labels == NULL) != (b->labels == NULL) ||
        (a->class_alpha == NULL) != (b->class_alpha == NULL) ||
        (a->class_defer == NULL) != (b->class_defer == NULL))
        return 0;
    for (i = 0; i < a->nclasses; i++) {
        if (a->labels != NULL && !sameString(a->labels[i], b->labels[i])) return 0;
        if (a->class_alpha != NULL && a->class_alpha[i] != b->class_alpha[i]) return 0;
        if (a->class_defer != NULL && a->class_defer[i] != b->class_defer[i]) return 0;
    }
    return 1;
}

TEST(create_option_order_does_not_matter) {
    uint64_t rng = 0xa54ff53a5f1d36f1ull;
    gateConfig ref, cfg;
    int order[FULL_GROUPS], i, round;

    for (i = 0; i < FULL_GROUPS; i++) order[i] = i;
    CHECK_EQ_INT(createGroups(order, &ref), GATE_OK);
    if (configIsZero(&ref)) return;

    /* Reversed: every option that depends on another one (class options on
     * CLASSES and ALPHA, dimensions on INPUTS, everything on MODE) now comes
     * before it. */
    for (i = 0; i < FULL_GROUPS; i++) order[i] = FULL_GROUPS - 1 - i;
    CHECK_EQ_INT(createGroups(order, &cfg), GATE_OK);
    CHECK(configsEqual(&ref, &cfg));
    gateConfigFree(&cfg);

    for (round = 0; round < 300; round++) {
        for (i = FULL_GROUPS - 1; i > 0; i--) { /* Fisher-Yates */
            int j = (int)(gate_test_rand64(&rng) % (uint64_t)(i + 1));
            int tmp = order[i];

            order[i] = order[j];
            order[j] = tmp;
        }
        CHECK_EQ_INT(createGroups(order, &cfg), GATE_OK);
        CHECK(configsEqual(&ref, &cfg));
        gateConfigFree(&cfg);
    }
    gateConfigFree(&ref);
}

TEST(create_cold_audit_follows_audit) {
    gateConfig cfg;

    CREATE_OK_OR_RETURN("MODE SCORE ALPHA 0.1 AUDIT 0.2", &cfg);
    CHECK(cfg.audit == 0.2 && cfg.cold_audit == 0.2);
    gateConfigFree(&cfg);

    CREATE_OK_OR_RETURN("MODE SCORE ALPHA 0.1 AUDIT 1", &cfg);
    CHECK(cfg.audit == 1.0 && cfg.cold_audit == 1.0);
    gateConfigFree(&cfg);

    CREATE_OK_OR_RETURN("MODE SCORE ALPHA 0.1 COLDAUDIT 0.3", &cfg);
    CHECK(cfg.audit == 0.05 && cfg.cold_audit == 0.3);
    gateConfigFree(&cfg);

    CREATE_OK_OR_RETURN("MODE SCORE ALPHA 0.1 AUDIT 0.2 COLDAUDIT 0.3", &cfg);
    CHECK(cfg.audit == 0.2 && cfg.cold_audit == 0.3);
    gateConfigFree(&cfg);

    /* An explicit COLDAUDIT is not overwritten by an AUDIT that comes later. */
    CREATE_OK_OR_RETURN("MODE SCORE ALPHA 0.1 COLDAUDIT 0.3 AUDIT 0.2", &cfg);
    CHECK(cfg.audit == 0.2 && cfg.cold_audit == 0.3);
    gateConfigFree(&cfg);

    /* ... not even when it is written with the default value of AUDIT. */
    CREATE_OK_OR_RETURN("MODE SCORE ALPHA 0.1 COLDAUDIT 0.05 AUDIT 0.2", &cfg);
    CHECK(cfg.audit == 0.2 && cfg.cold_audit == 0.05);
    gateConfigFree(&cfg);
}

TEST(create_is_case_insensitive_for_tokens_only) {
    gateConfig ref, cfg;

    CREATE_OK_OR_RETURN("MODE CLASSIFIER ALPHA 0.1 CLASSES 2 Yes no INPUTS 3 TEXT VECTOR "
                        "FEATURES VECTORDIM 4 CLASSALPHA 1 Yes 0.05 ALWAYSDEFER 1 no "
                        "ONMISSING ERROR EXPAND YES TEACHER T SCHEMAVERSION S", &ref);
    CHECK_EQ_INT(createLine("mode classifier alpha 0.1 classes 2 Yes no inputs 3 text vector "
                            "features vectordim 4 classalpha 1 Yes 0.05 alwaysdefer 1 no "
                            "onmissing error expand yes teacher T schemaversion S", &cfg),
                 GATE_OK);
    CHECK(configsEqual(&ref, &cfg));
    gateConfigFree(&cfg);
    CHECK_EQ_INT(createLine("Mode Classifier aLPHA 0.1 Classes 2 Yes no InPuTs 3 Text vECTOR "
                            "Features VectorDim 4 ClassAlpha 1 Yes 0.05 AlwaysDefer 1 no "
                            "OnMissing Error Expand Yes Teacher T SchemaVersion S", &cfg),
                 GATE_OK);
    CHECK(configsEqual(&ref, &cfg));
    gateConfigFree(&cfg);
    gateConfigFree(&ref);

    /* Labels and versions are data: their case is kept and it matters. */
    CREATE_OK_OR_RETURN("MODE SCORE ALPHA 0.1 CLASSES 2 a A TEACHER Teacher-V1", &cfg);
    CHECK(labelIs(&cfg, 0, "a"));
    CHECK(labelIs(&cfg, 1, "A"));
    CHECK_EQ_STR(cfg.teacher, "Teacher-V1");
    gateConfigFree(&cfg);
    CREATE_FAILS("MODE SCORE ALPHA 0.1 CLASSES 2 yes no CLASSALPHA 1 YES 0.05",
                 "unknown class label 'YES'");
    CREATE_FAILS("MODE SCORE ALPHA 0.1 CLASSES 2 yes no ALWAYSDEFER 1 No",
                 "unknown class label 'No'");
}

TEST(create_requires_mode_and_alpha) {
    testArgs none;

    argsInit(&none);
    CREATE_ARGS_FAIL(&none, "missing required option MODE");
    CREATE_FAILS("ALPHA 0.1", "missing required option MODE");
    CREATE_FAILS("MODE SCORE", "missing required option ALPHA");
    CREATE_FAILS("MODE CLASSIFIER CLASSES 2 a b INPUTS 1 TEXT", "missing required option ALPHA");
    CREATE_FAILS("TEACHER t SCHEMAVERSION s", "missing required option MODE");
    CREATE_FAILS("MODE BOTH ALPHA 0.1", "MODE must be SCORE or CLASSIFIER");
    CREATE_FAILS("MODE SCORES ALPHA 0.1", "MODE must be SCORE or CLASSIFIER");
    CREATE_FAILS("MODE 0 ALPHA 0.1", "MODE must be SCORE or CLASSIFIER");
}

/* Every option with a value that is valid on its own. */
static const struct {
    const char *name;
    const char *value;
    int min_args; /* fewest arguments it can be followed by, per the grammar */
} g_options[] = {
    {"MODE", "SCORE", 1},
    {"ALPHA", "0.1", 1},
    {"DELTA", "0.01", 1},
    {"AUDIT", "0.1", 1},
    {"COLDAUDIT", "0.1", 1},
    {"THRESHOLDS", "8", 1},
    {"CLASSES", "2 a b", 3},        /* count + two labels */
    {"CLASSALPHA", "1 a 0.05", 3},  /* count + one label/alpha pair */
    {"ALWAYSDEFER", "1 a", 2},      /* count + one label */
    {"INPUTS", "1 TEXT", 2},        /* count + one input */
    {"TEXTDIM", "64", 1},
    {"VECTORDIM", "4", 1},
    {"FEATUREDIM", "16", 1},
    {"LR", "0.1", 1},
    {"L2", "0", 1},
    {"TEACHER", "t", 1},
    {"SCHEMAVERSION", "s", 1},
    {"MAXMEMORY", "1000000", 1},
    {"MAXPENDING", "8", 1},
    {"OBSERVATIONTTL", "1000", 1},
    {"CERTTTL", "1000", 1},
    {"DRIFTMARGIN", "0.1", 1},
    {"ONMISSING", "REVOKE", 1},
    {"EXPAND", "NO", 1},
};
#define OPTION_COUNT (sizeof(g_options) / sizeof(g_options[0]))

static void lowerCase(char *dst, const char *src) {
    size_t i;

    for (i = 0; src[i] != '\0'; i++)
        dst[i] = (char)(src[i] >= 'A' && src[i] <= 'Z' ? src[i] - 'A' + 'a' : src[i]);
    dst[i] = '\0';
}

TEST(create_rejects_repeated_options) {
    size_t i;

    CHECK_EQ_INT(OPTION_COUNT, 24);
    for (i = 0; i < OPTION_COUNT; i++) {
        char line[256], msg[128], lower[32];

        snprintf(msg, sizeof(msg), "option %s given more than once", g_options[i].name);
        snprintf(line, sizeof(line), "%s %s %s %s", g_options[i].name, g_options[i].value,
                 g_options[i].name, g_options[i].value);
        CREATE_FAILS(line, msg);

        /* Another spelling of the same option, and no value at all: it is
         * still the repetition that is reported. */
        lowerCase(lower, g_options[i].name);
        snprintf(line, sizeof(line), "%s %s %s", g_options[i].name, g_options[i].value, lower);
        CREATE_FAILS(line, msg);
    }
    CREATE_FAILS("MODE SCORE ALPHA 0.1 TEACHER a DELTA 0.5 ALPHA 0.1",
                 "option ALPHA given more than once");
    CREATE_FAILS("MODE SCORE ALPHA 0.1 MODE CLASSIFIER", "option MODE given more than once");
}

TEST(create_rejects_truncated_commands) {
    size_t i;

    for (i = 0; i < OPTION_COUNT; i++) {
        char line[256], msg[128];

        snprintf(msg, sizeof(msg), "option %s requires %d argument(s)", g_options[i].name,
                 g_options[i].min_args);
        /* The option is the whole command, then the last token of one. */
        CREATE_FAILS(g_options[i].name, msg);
        if (strcmp(g_options[i].name, "TEACHER") != 0) {
            snprintf(line, sizeof(line), "TEACHER t %s", g_options[i].name);
            CREATE_FAILS(line, msg);
        }
    }

    /* Lists: fewer arguments than the shortest legal list ... */
    CREATE_FAILS("MODE SCORE ALPHA 0.1 CLASSES 2", "option CLASSES requires 3 argument(s)");
    CREATE_FAILS("MODE SCORE ALPHA 0.1 CLASSES 2 a", "option CLASSES requires 3 argument(s)");
    CREATE_FAILS("MODE SCORE ALPHA 0.1 CLASSALPHA 1 a", "option CLASSALPHA requires 3 argument(s)");
    CREATE_FAILS("MODE SCORE ALPHA 0.1 ALWAYSDEFER 1", "option ALWAYSDEFER requires 2 argument(s)");
    CREATE_FAILS("MODE SCORE ALPHA 0.1 INPUTS 1", "option INPUTS requires 2 argument(s)");
    /* ... and fewer than their count announces. */
    CREATE_FAILS("MODE SCORE ALPHA 0.1 CLASSES 3 a b",
                 "CLASSES count 3 needs 3 argument(s) after it but only 2 follow");
    CREATE_FAILS("MODE SCORE ALPHA 0.1 CLASSES 64 a b c",
                 "CLASSES count 64 needs 64 argument(s) after it but only 3 follow");
    CREATE_FAILS("MODE SCORE ALPHA 0.1 CLASSES 2 a b CLASSALPHA 2 a 0.05 b",
                 "CLASSALPHA count 2 needs 4 argument(s) after it but only 3 follow");
    CREATE_FAILS("MODE SCORE ALPHA 0.1 CLASSES 2 a b CLASSALPHA 2 a 0.05",
                 "CLASSALPHA count 2 needs 4 argument(s) after it but only 2 follow");
    CREATE_FAILS("MODE SCORE ALPHA 0.1 CLASSES 3 a b c ALWAYSDEFER 3 a b",
                 "ALWAYSDEFER count 3 needs 3 argument(s) after it but only 2 follow");
    CREATE_FAILS("MODE CLASSIFIER ALPHA 0.1 CLASSES 2 a b INPUTS 2 TEXT",
                 "INPUTS count 2 needs 2 argument(s) after it but only 1 follow");
    CREATE_FAILS("MODE CLASSIFIER ALPHA 0.1 CLASSES 2 a b INPUTS 3 TEXT VECTOR",
                 "INPUTS count 3 needs 3 argument(s) after it but only 2 follow");
}

TEST(create_rejects_unknown_tokens) {
    testArgs a;

    CREATE_FAILS("FOO", "unknown option 'FOO'");
    CREATE_FAILS("MODE SCORE ALPHA 0.1 FOO", "unknown option 'FOO'");
    CREATE_FAILS("MODE SCORE ALPHA 0.1 FOO bar", "unknown option 'FOO'");
    CREATE_FAILS("MODE SCORE 0.1", "unknown option '0.1'");
    CREATE_FAILS("MODES SCORE ALPHA 0.1", "unknown option 'MODES'");
    CREATE_FAILS("MOD SCORE ALPHA 0.1", "unknown option 'MOD'");
    CREATE_FAILS("MODE SCORE ALPHA=0.1", "unknown option 'ALPHA=0.1'");
    CREATE_FAILS("MODE SCORE --alpha 0.1", "unknown option '--alpha'");
    /* Tokens of other commands are not GCREATE options. */
    CREATE_FAILS("MODE SCORE ALPHA 0.1 TEXT hello", "unknown option 'TEXT'");
    CREATE_FAILS("MODE SCORE ALPHA 0.1 LABEL x", "unknown option 'LABEL'");
    CREATE_FAILS("MODE SCORE ALPHA 0.1 SET AUDIT 0.2", "unknown option 'SET'");
    /* A value that was not consumed by an option is not skipped. */
    CREATE_FAILS("MODE SCORE ALPHA 0.1 0.2", "unknown option '0.2'");
    CREATE_FAILS("MODE SCORE ALPHA 0.1 EXPAND YES NO", "unknown option 'NO'");

    /* The empty argument, and an option name with a trailing NUL. */
    argsInit(&a);
    argsSplit(&a, "MODE SCORE ALPHA 0.1");
    argsPushLen(&a, "", 0);
    CREATE_ARGS_FAIL(&a, "unknown option ''");
    argsFree(&a);
    argsSplit(&a, "MODE SCORE");
    argsPushLen(&a, LIT("ALPHA\0"));
    argsPush(&a, "0.1");
    CREATE_ARGS_FAIL(&a, "unknown option 'ALPHA?'");
    argsFree(&a);
}

TEST(create_labels_may_look_like_options) {
    gateConfig cfg;

    /* The count delimits the list: ALPHA, MODE and CLASSES are labels here,
     * and the real ALPHA option follows. */
    CREATE_OK_OR_RETURN("MODE CLASSIFIER CLASSES 3 ALPHA MODE CLASSES INPUTS 1 TEXT ALPHA 0.1",
                        &cfg);
    CHECK_EQ_INT(cfg.nclasses, 3);
    CHECK(labelIs(&cfg, 0, "ALPHA"));
    CHECK(labelIs(&cfg, 1, "MODE"));
    CHECK(labelIs(&cfg, 2, "CLASSES"));
    CHECK(cfg.alpha == 0.1);
    CHECK_EQ_INT(cfg.schema.inputs, GATE_INPUT_TEXT);
    gateConfigFree(&cfg);

    /* "INPUTS" and "1" are labels; the second INPUTS is the option. */
    CREATE_OK_OR_RETURN("MODE CLASSIFIER ALPHA 0.1 CLASSES 2 INPUTS 1 INPUTS 1 TEXT", &cfg);
    CHECK(labelIs(&cfg, 0, "INPUTS"));
    CHECK(labelIs(&cfg, 1, "1"));
    CHECK_EQ_INT(cfg.schema.inputs, GATE_INPUT_TEXT);
    gateConfigFree(&cfg);

    /* The same holds for the labels of the class options. */
    CREATE_OK_OR_RETURN("MODE SCORE CLASSALPHA 1 ALPHA 0.05 ALWAYSDEFER 1 EXPAND "
                        "CLASSES 3 ALPHA EXPAND TEACHER ALPHA 0.1", &cfg);
    CHECK(labelIs(&cfg, 0, "ALPHA"));
    CHECK(labelIs(&cfg, 1, "EXPAND"));
    CHECK(labelIs(&cfg, 2, "TEACHER"));
    CHECK(cfg.alpha == 0.1);
    CHECK_EQ_STR(cfg.teacher, "dev"); /* "TEACHER" was a label, not the option */
    CHECK_EQ_INT(cfg.expand, 0);
    CHECK(cfg.class_alpha != NULL && cfg.class_defer != NULL);
    if (cfg.class_alpha != NULL && cfg.class_defer != NULL) {
        CHECK(cfg.class_alpha[0] == 0.05);
        CHECK(cfg.class_alpha[1] == 0.1);
        CHECK(cfg.class_alpha[2] == 0.1);
        CHECK_EQ_INT(cfg.class_defer[0], 0);
        CHECK_EQ_INT(cfg.class_defer[1], 1);
        CHECK_EQ_INT(cfg.class_defer[2], 0);
    }
    gateConfigFree(&cfg);

    /* A count one too large swallows the next option name as a label. */
    CREATE_FAILS("MODE CLASSIFIER ALPHA 0.1 CLASSES 3 a b INPUTS 1 TEXT", "unknown option '1'");
    /* A count one too small leaves a label where an option is expected. */
    CREATE_FAILS("MODE CLASSIFIER ALPHA 0.1 CLASSES 2 a b c INPUTS 1 TEXT", "unknown option 'c'");
}

TEST(create_number_syntax_errors) {
    static const char *const doubles_score[] = {"ALPHA", "DELTA", "AUDIT", "COLDAUDIT",
                                                "DRIFTMARGIN"};
    static const char *const doubles_classifier[] = {"LR", "L2"};
    static const char *const bad_doubles[] = {"abc", "nan", "inf", "-inf", "0x1", "1e999",
                                              "1e", "0,5", "50%"};
    static const char *const integers[] = {"THRESHOLDS", "MAXMEMORY", "MAXPENDING",
                                           "OBSERVATIONTTL", "CERTTTL"};
    static const char *const dims[] = {"TEXTDIM", "VECTORDIM", "FEATUREDIM"};
    static const char *const bad_integers[] = {"abc", "-1", "+1", "1.5", "1e3", "0x10", "8k"};
    char line[256], msg[128];
    testArgs a;
    size_t i, j;

    for (i = 0; i < sizeof(doubles_score) / sizeof(doubles_score[0]); i++) {
        snprintf(msg, sizeof(msg), "%s must be a finite decimal number", doubles_score[i]);
        for (j = 0; j < sizeof(bad_doubles) / sizeof(bad_doubles[0]); j++) {
            /* The option under test comes first: ALPHA is given once only. */
            if (strcmp(doubles_score[i], "ALPHA") == 0)
                snprintf(line, sizeof(line), "ALPHA %s MODE SCORE", bad_doubles[j]);
            else
                snprintf(line, sizeof(line), "%s %s MODE SCORE ALPHA 0.1", doubles_score[i],
                         bad_doubles[j]);
            CREATE_FAILS(line, msg);
        }
        /* The empty argument and one with white space around a number. */
        argsInit(&a);
        argsPush(&a, doubles_score[i]);
        argsPushLen(&a, "", 0);
        CREATE_ARGS_FAIL(&a, msg);
        argsFree(&a);
        argsPush(&a, doubles_score[i]);
        argsPush(&a, " 0.1");
        CREATE_ARGS_FAIL(&a, msg);
        argsFree(&a);
        argsPush(&a, doubles_score[i]);
        argsPush(&a, "0.1 ");
        CREATE_ARGS_FAIL(&a, msg);
        argsFree(&a);
    }
    for (i = 0; i < sizeof(doubles_classifier) / sizeof(doubles_classifier[0]); i++) {
        snprintf(msg, sizeof(msg), "%s must be a finite decimal number", doubles_classifier[i]);
        for (j = 0; j < sizeof(bad_doubles) / sizeof(bad_doubles[0]); j++) {
            snprintf(line, sizeof(line),
                     "MODE CLASSIFIER ALPHA 0.1 CLASSES 2 a b INPUTS 1 TEXT %s %s",
                     doubles_classifier[i], bad_doubles[j]);
            CREATE_FAILS(line, msg);
        }
    }

    for (i = 0; i < sizeof(integers) / sizeof(integers[0]); i++) {
        snprintf(msg, sizeof(msg), "%s must be an unsigned integer", integers[i]);
        for (j = 0; j < sizeof(bad_integers) / sizeof(bad_integers[0]); j++) {
            snprintf(line, sizeof(line), "MODE SCORE ALPHA 0.1 %s %s", integers[i],
                     bad_integers[j]);
            CREATE_FAILS(line, msg);
        }
        argsInit(&a);
        argsPush(&a, integers[i]);
        argsPushLen(&a, "", 0);
        CREATE_ARGS_FAIL(&a, msg);
        argsFree(&a);
        argsPush(&a, integers[i]);
        argsPush(&a, " 8");
        CREATE_ARGS_FAIL(&a, msg);
        argsFree(&a);
    }
    for (i = 0; i < sizeof(dims) / sizeof(dims[0]); i++) {
        snprintf(msg, sizeof(msg), "%s must be an unsigned integer", dims[i]);
        for (j = 0; j < sizeof(bad_integers) / sizeof(bad_integers[0]); j++) {
            snprintf(line, sizeof(line),
                     "MODE CLASSIFIER ALPHA 0.1 CLASSES 2 a b INPUTS 3 TEXT VECTOR FEATURES %s %s",
                     dims[i], bad_integers[j]);
            CREATE_FAILS(line, msg);
        }
    }

    CREATE_FAILS("MODE SCORE ALPHA 0.1 CLASSES x a b", "CLASSES count must be an unsigned integer");
    CREATE_FAILS("MODE SCORE ALPHA 0.1 CLASSES -2 a b", "CLASSES count must be an unsigned integer");
    CREATE_FAILS("MODE SCORE ALPHA 0.1 CLASSES a b c", "CLASSES count must be an unsigned integer");
    CREATE_FAILS("MODE SCORE ALPHA 0.1 CLASSES 2 a b CLASSALPHA a 0.05 b",
                 "CLASSALPHA count must be an unsigned integer");
    CREATE_FAILS("MODE SCORE ALPHA 0.1 CLASSES 2 a b ALWAYSDEFER a b",
                 "ALWAYSDEFER count must be an unsigned integer");
    CREATE_FAILS("MODE CLASSIFIER ALPHA 0.1 CLASSES 2 a b INPUTS TEXT FEATURES",
                 "INPUTS count must be an unsigned integer");
    CREATE_FAILS("MODE SCORE ALPHA 0.1 ONMISSING IGNORE", "ONMISSING must be REVOKE or ERROR");
    CREATE_FAILS("MODE SCORE ALPHA 0.1 ONMISSING 1", "ONMISSING must be REVOKE or ERROR");
    CREATE_FAILS("MODE SCORE ALPHA 0.1 EXPAND 1", "EXPAND must be NO or YES");
    CREATE_FAILS("MODE SCORE ALPHA 0.1 EXPAND TRUE", "EXPAND must be NO or YES");
    CREATE_FAILS("MODE SCORE ALPHA 0.1 EXPAND YESS", "EXPAND must be NO or YES");
}

TEST(create_double_ranges) {
    gateConfig cfg;

    /* Owned by the validator: only the distinctive part is matched. */
    CREATE_FAILS_WITH("MODE SCORE ALPHA 0", "ALPHA must be greater than 0");
    CREATE_FAILS_WITH("MODE SCORE ALPHA 1", "ALPHA must be greater than 0");
    CREATE_FAILS_WITH("MODE SCORE ALPHA 1.5", "ALPHA must be greater than 0");
    CREATE_FAILS_WITH("MODE SCORE ALPHA -0.1", "ALPHA must be greater than 0");
    /* Underflow is a value (zero), so it gets the range message. */
    CREATE_FAILS_WITH("MODE SCORE ALPHA 1e-999", "ALPHA must be greater than 0");
    CREATE_FAILS_WITH("MODE SCORE ALPHA 0.1 DELTA 0", "DELTA must be greater than 0");
    CREATE_FAILS_WITH("MODE SCORE ALPHA 0.1 DELTA 1", "DELTA must be greater than 0");
    CREATE_FAILS_WITH("MODE SCORE ALPHA 0.1 AUDIT 0", "AUDIT must be greater than 0");
    CREATE_FAILS_WITH("MODE SCORE ALPHA 0.1 AUDIT 1.5", "AUDIT must be greater than 0");
    CREATE_FAILS_WITH("MODE SCORE ALPHA 0.1 AUDIT -1", "AUDIT must be greater than 0");
    CREATE_FAILS_WITH("MODE SCORE ALPHA 0.1 COLDAUDIT 0", "COLDAUDIT must be greater than 0");
    CREATE_FAILS_WITH("MODE SCORE ALPHA 0.1 COLDAUDIT 2", "COLDAUDIT must be greater than 0");
    CREATE_FAILS_WITH("MODE CLASSIFIER ALPHA 0.1 CLASSES 2 a b INPUTS 1 TEXT LR 0",
                      "LR must be greater than 0");
    CREATE_FAILS_WITH("MODE CLASSIFIER ALPHA 0.1 CLASSES 2 a b INPUTS 1 TEXT LR -1",
                      "LR must be greater than 0");
    CREATE_FAILS_WITH("MODE CLASSIFIER ALPHA 0.1 CLASSES 2 a b INPUTS 1 TEXT LR 10.5",
                      "LR must be greater than 0");
    CREATE_FAILS_WITH("MODE CLASSIFIER ALPHA 0.1 CLASSES 2 a b INPUTS 1 TEXT L2 -0.5",
                      "L2 must be");
    CREATE_FAILS_WITH("MODE CLASSIFIER ALPHA 0.1 CLASSES 2 a b INPUTS 1 TEXT LR 2 L2 0.5",
                      "LR * L2");

    CREATE_OK_OR_RETURN("MODE SCORE ALPHA 0.999 DELTA 0.5 AUDIT 1 COLDAUDIT 0.001", &cfg);
    CHECK(cfg.alpha == 0.999 && cfg.delta == 0.5 && cfg.audit == 1.0 && cfg.cold_audit == 0.001);
    gateConfigFree(&cfg);
    CREATE_OK_OR_RETURN("MODE CLASSIFIER ALPHA 0.1 CLASSES 2 a b INPUTS 1 TEXT LR 10 L2 0.09",
                        &cfg);
    CHECK(cfg.lr == 10.0 && cfg.l2 == 0.09);
    gateConfigFree(&cfg);

    /* DRIFTMARGIN is checked by the parser: in the config a negative margin
     * means "not given", so "DRIFTMARGIN -1" must not pass as the default. */
    CREATE_FAILS("MODE SCORE ALPHA 0.1 DRIFTMARGIN -1", "DRIFTMARGIN must be between 0 and 1");
    CREATE_FAILS("MODE SCORE ALPHA 0.1 DRIFTMARGIN -0.1", "DRIFTMARGIN must be between 0 and 1");
    CREATE_FAILS("MODE SCORE ALPHA 0.1 DRIFTMARGIN -1e-9", "DRIFTMARGIN must be between 0 and 1");
    CREATE_FAILS("MODE SCORE ALPHA 0.1 DRIFTMARGIN 1.5", "DRIFTMARGIN must be between 0 and 1");
    CREATE_FAILS("MODE SCORE ALPHA 0.1 DRIFTMARGIN 1.0000001",
                 "DRIFTMARGIN must be between 0 and 1");
    CREATE_OK_OR_RETURN("MODE SCORE ALPHA 0.1 DRIFTMARGIN 0", &cfg);
    CHECK(cfg.drift_margin == 0.0 && !signbit(cfg.drift_margin));
    gateConfigFree(&cfg);
    CREATE_OK_OR_RETURN("MODE SCORE ALPHA 0.1 DRIFTMARGIN 1", &cfg);
    CHECK(cfg.drift_margin == 1.0);
    gateConfigFree(&cfg);
    CREATE_OK_OR_RETURN("MODE SCORE ALPHA 0.1 DRIFTMARGIN 0.5", &cfg);
    CHECK(cfg.drift_margin == 0.5);
    gateConfigFree(&cfg);
    /* An explicit zero margin is never stored with the sign of the default. */
    CREATE_OK_OR_RETURN("MODE SCORE ALPHA 0.1 DRIFTMARGIN -0", &cfg);
    CHECK(cfg.drift_margin == 0.0 && !signbit(cfg.drift_margin));
    gateConfigFree(&cfg);
}

TEST(create_integer_ranges) {
    gateConfig cfg;
    const char *msg;

    /* Each rejected value is picked so that a silent truncation to the
     * width of the config field would give a value that is VALID:
     * 2^32 + 2 -> 2, 2^32 + 8192 -> 8192, 2^63 -> a negative TTL, ... */
    msg = "THRESHOLDS is out of range: must be between 2 and 256";
    CREATE_FAILS("MODE SCORE ALPHA 0.1 THRESHOLDS 0", msg);
    CREATE_FAILS("MODE SCORE ALPHA 0.1 THRESHOLDS 1", msg);
    CREATE_FAILS("MODE SCORE ALPHA 0.1 THRESHOLDS 257", msg);
    CREATE_FAILS("MODE SCORE ALPHA 0.1 THRESHOLDS 4294967298", msg);
    CREATE_FAILS("MODE SCORE ALPHA 0.1 THRESHOLDS 18446744073709551616", msg);
    CREATE_FAILS("MODE SCORE ALPHA 0.1 THRESHOLDS 99999999999999999999999", msg);
    CREATE_OK_OR_RETURN("MODE SCORE ALPHA 0.1 THRESHOLDS 2", &cfg);
    CHECK_EQ_INT(cfg.nthresholds, 2);
    gateConfigFree(&cfg);
    CREATE_OK_OR_RETURN("MODE SCORE ALPHA 0.1 THRESHOLDS 256", &cfg);
    CHECK_EQ_INT(cfg.nthresholds, 256);
    gateConfigFree(&cfg);

    msg = "MAXPENDING is out of range: must be between 1 and 1048576";
    CREATE_FAILS("MODE SCORE ALPHA 0.1 MAXPENDING 0", msg);
    CREATE_FAILS("MODE SCORE ALPHA 0.1 MAXPENDING 1048577", msg);
    CREATE_FAILS("MODE SCORE ALPHA 0.1 MAXPENDING 4294967297", msg);
    CREATE_FAILS("MODE SCORE ALPHA 0.1 MAXPENDING 18446744073709551615", msg);
    CREATE_OK_OR_RETURN("MODE SCORE ALPHA 0.1 MAXPENDING 1", &cfg);
    CHECK_EQ_INT(cfg.max_pending, 1);
    gateConfigFree(&cfg);
    CREATE_OK_OR_RETURN("MODE SCORE ALPHA 0.1 MAXPENDING 1048576", &cfg);
    CHECK_EQ_INT(cfg.max_pending, 1048576);
    gateConfigFree(&cfg);

    msg = "OBSERVATIONTTL is out of range: must be between 1 and 315360000000 ms";
    CREATE_FAILS("MODE SCORE ALPHA 0.1 OBSERVATIONTTL 0", msg);
    CREATE_FAILS("MODE SCORE ALPHA 0.1 OBSERVATIONTTL 315360000001", msg);
    CREATE_FAILS("MODE SCORE ALPHA 0.1 OBSERVATIONTTL 9223372036854775808", msg);
    CREATE_FAILS("MODE SCORE ALPHA 0.1 OBSERVATIONTTL 18446744073709551615", msg);
    CREATE_FAILS("MODE SCORE ALPHA 0.1 OBSERVATIONTTL 18446744073709551616", msg);
    msg = "CERTTTL is out of range: must be between 1 and 315360000000 ms";
    CREATE_FAILS("MODE SCORE ALPHA 0.1 CERTTTL 0", msg);
    CREATE_FAILS("MODE SCORE ALPHA 0.1 CERTTTL 315360000001", msg);
    CREATE_FAILS("MODE SCORE ALPHA 0.1 CERTTTL 9223372036854775808", msg);
    CREATE_FAILS("MODE SCORE ALPHA 0.1 CERTTTL 18446744073709551615", msg);
    CREATE_OK_OR_RETURN("MODE SCORE ALPHA 0.1 OBSERVATIONTTL 1 CERTTTL 315360000000", &cfg);
    CHECK_EQ_INT(cfg.observation_ttl_ms, 1);
    CHECK_EQ_INT(cfg.cert_ttl_ms, 315360000000ll);
    gateConfigFree(&cfg);
    CREATE_OK_OR_RETURN("MODE SCORE ALPHA 0.1 OBSERVATIONTTL 315360000000 CERTTTL 1", &cfg);
    CHECK_EQ_INT(cfg.observation_ttl_ms, 315360000000ll);
    CHECK_EQ_INT(cfg.cert_ttl_ms, 1);
    gateConfigFree(&cfg);

    msg = "MAXMEMORY is out of range: must be at most 268435456 bytes";
    CREATE_FAILS("MODE SCORE ALPHA 0.1 MAXMEMORY 268435457", msg);
    CREATE_FAILS("MODE SCORE ALPHA 0.1 MAXMEMORY 18446744073709551615", msg);
    CREATE_FAILS("MODE SCORE ALPHA 0.1 MAXMEMORY 18446744073709551616", msg);
    CREATE_OK_OR_RETURN("MODE SCORE ALPHA 0.1 MAXMEMORY 268435456", &cfg);
    CHECK_EQ_INT(cfg.max_memory, 268435456);
    gateConfigFree(&cfg);

#define CLS "MODE CLASSIFIER ALPHA 0.1 CLASSES 2 a b INPUTS 3 TEXT VECTOR FEATURES "
    msg = "TEXTDIM is out of range: must be between 1 and 1048576";
    CREATE_FAILS(CLS "VECTORDIM 4 TEXTDIM 0", msg);
    CREATE_FAILS(CLS "VECTORDIM 4 TEXTDIM 1048577", msg);
    CREATE_FAILS(CLS "VECTORDIM 4 TEXTDIM 4294975488", msg);
    msg = "VECTORDIM is out of range: must be between 1 and 16384";
    CREATE_FAILS(CLS "VECTORDIM 0", msg);
    CREATE_FAILS(CLS "VECTORDIM 16385", msg);
    CREATE_FAILS(CLS "VECTORDIM 4294967304", msg);
    msg = "FEATUREDIM is out of range: must be between 1 and 65536";
    CREATE_FAILS(CLS "VECTORDIM 4 FEATUREDIM 0", msg);
    CREATE_FAILS(CLS "VECTORDIM 4 FEATUREDIM 65537", msg);
    CREATE_FAILS(CLS "VECTORDIM 4 FEATUREDIM 4294967552", msg);
    CREATE_OK_OR_RETURN(CLS "TEXTDIM 1 VECTORDIM 1 FEATUREDIM 1", &cfg);
    CHECK_EQ_INT(cfg.schema.text_dim, 1);
    CHECK_EQ_INT(cfg.schema.vector_dim, 1);
    CHECK_EQ_INT(cfg.schema.feature_dim, 1);
    gateConfigFree(&cfg);
    CREATE_OK_OR_RETURN(CLS "TEXTDIM 1048576 VECTORDIM 16384 FEATUREDIM 65536", &cfg);
    CHECK_EQ_INT(cfg.schema.text_dim, 1048576);
    CHECK_EQ_INT(cfg.schema.vector_dim, 16384);
    CHECK_EQ_INT(cfg.schema.feature_dim, 65536);
    gateConfigFree(&cfg);
#undef CLS

    msg = "CLASSES count is out of range: must be between 2 and 64";
    CREATE_FAILS("MODE SCORE ALPHA 0.1 CLASSES 0 a b", msg);
    CREATE_FAILS("MODE SCORE ALPHA 0.1 CLASSES 1 a b", msg);
    CREATE_FAILS("MODE SCORE ALPHA 0.1 CLASSES 65 a b", msg);
    CREATE_FAILS("MODE SCORE ALPHA 0.1 CLASSES 4294967298 a b", msg);
    msg = "INPUTS count is out of range: must be between 1 and 3";
    CREATE_FAILS("MODE CLASSIFIER ALPHA 0.1 CLASSES 2 a b INPUTS 0 TEXT", msg);
    CREATE_FAILS("MODE CLASSIFIER ALPHA 0.1 CLASSES 2 a b INPUTS 4 TEXT VECTOR FEATURES TEXT",
                 msg);
    CREATE_FAILS("MODE CLASSIFIER ALPHA 0.1 CLASSES 2 a b INPUTS 4294967297 TEXT", msg);
    msg = "CLASSALPHA count is out of range: must be between 1 and 64";
    CREATE_FAILS("MODE SCORE ALPHA 0.1 CLASSES 2 a b CLASSALPHA 0 a 0.05", msg);
    CREATE_FAILS("MODE SCORE ALPHA 0.1 CLASSES 2 a b CLASSALPHA 65 a 0.05", msg);
    CREATE_FAILS("MODE SCORE ALPHA 0.1 CLASSES 2 a b CLASSALPHA 4294967297 a 0.05", msg);
    msg = "ALWAYSDEFER count is out of range: must be between 1 and 64";
    CREATE_FAILS("MODE SCORE ALPHA 0.1 CLASSES 2 a b ALWAYSDEFER 0 a", msg);
    CREATE_FAILS("MODE SCORE ALPHA 0.1 CLASSES 2 a b ALWAYSDEFER 65 a", msg);
    CREATE_FAILS("MODE SCORE ALPHA 0.1 CLASSES 2 a b ALWAYSDEFER 4294967297 a", msg);
}

TEST(create_class_count_limits) {
    gateConfig cfg;
    testArgs a;
    char label[16];
    int i, rc;

    /* 64 labels is the documented maximum. */
    argsInit(&a);
    argsSplit(&a, "MODE CLASSIFIER ALPHA 0.1 INPUTS 1 TEXT CLASSES 64");
    for (i = 0; i < 64; i++) {
        snprintf(label, sizeof(label), "class%d", i);
        argsPush(&a, label);
    }
    rc = createArgs(&a, &cfg);
    argsFree(&a);
    CHECK_EQ_INT(rc, GATE_OK);
    if (rc != GATE_OK) return;
    CHECK_EQ_INT(cfg.nclasses, 64);
    CHECK(labelIs(&cfg, 0, "class0"));
    CHECK(labelIs(&cfg, 37, "class37"));
    CHECK(labelIs(&cfg, 63, "class63"));
    gateConfigFree(&cfg);

    argsSplit(&a, "MODE CLASSIFIER ALPHA 0.1 INPUTS 1 TEXT CLASSES 65");
    for (i = 0; i < 65; i++) {
        snprintf(label, sizeof(label), "class%d", i);
        argsPush(&a, label);
    }
    CREATE_ARGS_FAIL(&a, "CLASSES count is out of range: must be between 2 and 64");
    argsFree(&a);

    /* A list option covering all 64 classes. */
    argsSplit(&a, "MODE SCORE ALPHA 0.1 CLASSES 64");
    for (i = 0; i < 64; i++) {
        snprintf(label, sizeof(label), "class%d", i);
        argsPush(&a, label);
    }
    argsSplit(&a, "CLASSALPHA 64");
    for (i = 0; i < 64; i++) {
        snprintf(label, sizeof(label), "class%d", 63 - i);
        argsPush(&a, label);
        snprintf(label, sizeof(label), "0.0%02d", 63 - i + 1); /* 0.001 .. 0.064 */
        argsPush(&a, label);
    }
    rc = createArgs(&a, &cfg);
    argsFree(&a);
    CHECK_EQ_INT(rc, GATE_OK);
    if (rc != GATE_OK) return;
    CHECK(cfg.class_alpha != NULL);
    if (cfg.class_alpha != NULL) {
        CHECK(cfg.class_alpha[0] == 0.001);
        CHECK(cfg.class_alpha[9] == 0.010);
        CHECK(cfg.class_alpha[63] == 0.064);
    }
    gateConfigFree(&cfg);
}

TEST(create_class_policy) {
    gateConfig cfg;

#define CLS "MODE CLASSIFIER ALPHA 0.1 INPUTS 1 TEXT "
    /* No class option: one pooled row, no per-class arrays. */
    CREATE_OK_OR_RETURN(CLS "CLASSES 3 a b c", &cfg);
    CHECK_EQ_INT(cfg.class_mode, 0);
    CHECK(cfg.class_alpha == NULL && cfg.class_defer == NULL);
    gateConfigFree(&cfg);

    /* CLASSALPHA after and before CLASSES. Unlisted classes get ALPHA. */
    CREATE_OK_OR_RETURN(CLS "CLASSES 3 a b c CLASSALPHA 1 b 0.05", &cfg);
    CHECK_EQ_INT(cfg.class_mode, 1);
    CHECK(cfg.class_alpha != NULL && cfg.class_defer != NULL);
    if (cfg.class_alpha != NULL && cfg.class_defer != NULL) {
        CHECK(cfg.class_alpha[0] == 0.1 && cfg.class_alpha[1] == 0.05 && cfg.class_alpha[2] == 0.1);
        CHECK(cfg.class_defer[0] == 0 && cfg.class_defer[1] == 0 && cfg.class_defer[2] == 0);
    }
    gateConfigFree(&cfg);
    CREATE_OK_OR_RETURN(CLS "CLASSALPHA 1 b 0.05 CLASSES 3 a b c", &cfg);
    CHECK_EQ_INT(cfg.class_mode, 1);
    if (cfg.class_alpha != NULL && cfg.class_defer != NULL) {
        CHECK(cfg.class_alpha[0] == 0.1 && cfg.class_alpha[1] == 0.05 && cfg.class_alpha[2] == 0.1);
        CHECK(cfg.class_defer[0] == 0 && cfg.class_defer[1] == 0 && cfg.class_defer[2] == 0);
    }
    gateConfigFree(&cfg);

    /* ALWAYSDEFER after and before CLASSES. */
    CREATE_OK_OR_RETURN(CLS "CLASSES 3 a b c ALWAYSDEFER 2 a c", &cfg);
    CHECK_EQ_INT(cfg.class_mode, 1);
    CHECK(cfg.class_alpha != NULL && cfg.class_defer != NULL);
    if (cfg.class_alpha != NULL && cfg.class_defer != NULL) {
        CHECK(cfg.class_alpha[0] == 0.1 && cfg.class_alpha[1] == 0.1 && cfg.class_alpha[2] == 0.1);
        CHECK(cfg.class_defer[0] == 1 && cfg.class_defer[1] == 0 && cfg.class_defer[2] == 1);
    }
    gateConfigFree(&cfg);
    CREATE_OK_OR_RETURN(CLS "ALWAYSDEFER 2 c a CLASSES 3 a b c", &cfg);
    if (cfg.class_alpha != NULL && cfg.class_defer != NULL) {
        CHECK(cfg.class_alpha[0] == 0.1 && cfg.class_alpha[1] == 0.1 && cfg.class_alpha[2] == 0.1);
        CHECK(cfg.class_defer[0] == 1 && cfg.class_defer[1] == 0 && cfg.class_defer[2] == 1);
    }
    gateConfigFree(&cfg);

    /* Both, in list order different from label order; a target equal to
     * ALPHA is allowed. */
    CREATE_OK_OR_RETURN(CLS "CLASSALPHA 2 b 0.1 a 0.01 ALWAYSDEFER 1 c CLASSES 3 a b c", &cfg);
    if (cfg.class_alpha != NULL && cfg.class_defer != NULL) {
        CHECK(cfg.class_alpha[0] == 0.01 && cfg.class_alpha[1] == 0.1 && cfg.class_alpha[2] == 0.1);
        CHECK(cfg.class_defer[0] == 0 && cfg.class_defer[1] == 0 && cfg.class_defer[2] == 1);
    }
    gateConfigFree(&cfg);
#undef CLS

    /* ALPHA after the class options: unlisted classes get the final ALPHA,
     * not the value the field had while the options were being read. */
    CREATE_OK_OR_RETURN("MODE SCORE CLASSES 2 a b CLASSALPHA 1 a 0.05 ALPHA 0.2", &cfg);
    if (cfg.class_alpha != NULL) CHECK(cfg.class_alpha[0] == 0.05 && cfg.class_alpha[1] == 0.2);
    gateConfigFree(&cfg);

    /* A SCORE gate with classes takes the class options too. */
    CREATE_OK_OR_RETURN("MODE SCORE ALPHA 0.1 CLASSES 2 yes no CLASSALPHA 1 yes 0.05 "
                        "ALWAYSDEFER 1 no", &cfg);
    CHECK_EQ_INT(cfg.mode, GATE_MODE_SCORE);
    CHECK_EQ_INT(cfg.class_mode, 1);
    if (cfg.class_alpha != NULL && cfg.class_defer != NULL) {
        CHECK(cfg.class_alpha[0] == 0.05 && cfg.class_alpha[1] == 0.1);
        CHECK(cfg.class_defer[0] == 0 && cfg.class_defer[1] == 1);
    }
    gateConfigFree(&cfg);

#define SC "MODE SCORE ALPHA 0.1 "
    /* Unknown labels, in either option, wherever CLASSES is. */
    CREATE_FAILS(SC "CLASSES 2 a b CLASSALPHA 1 zzz 0.05", "unknown class label 'zzz'");
    CREATE_FAILS(SC "CLASSALPHA 1 zzz 0.05 CLASSES 2 a b", "unknown class label 'zzz'");
    CREATE_FAILS(SC "CLASSES 2 a b CLASSALPHA 2 a 0.05 zzz 0.05", "unknown class label 'zzz'");
    CREATE_FAILS(SC "CLASSES 2 a b ALWAYSDEFER 1 zzz", "unknown class label 'zzz'");
    CREATE_FAILS(SC "ALWAYSDEFER 2 a zzz CLASSES 2 a b", "unknown class label 'zzz'");
    CREATE_FAILS(SC "CLASSES 2 a b ALWAYSDEFER 1 ab", "unknown class label 'ab'");
    CREATE_FAILS(SC "CLASSES 2 ab b ALWAYSDEFER 1 a", "unknown class label 'a'");

    /* A label twice in one option, and one label in both options. */
    CREATE_FAILS(SC "CLASSES 2 a b CLASSALPHA 2 a 0.05 a 0.06",
                 "class label 'a' listed more than once in CLASSALPHA");
    CREATE_FAILS(SC "CLASSES 3 a b c ALWAYSDEFER 2 b b",
                 "class label 'b' listed more than once in ALWAYSDEFER");
    CREATE_FAILS(SC "CLASSES 3 a b c CLASSALPHA 1 a 0.05 ALWAYSDEFER 1 a",
                 "class label 'a' cannot be in both CLASSALPHA and ALWAYSDEFER");
    CREATE_FAILS(SC "ALWAYSDEFER 2 b a CLASSALPHA 1 a 0.05 CLASSES 3 a b c",
                 "class label 'a' cannot be in both CLASSALPHA and ALWAYSDEFER");

    /* Class options without CLASSES. */
    CREATE_FAILS(SC "CLASSALPHA 1 a 0.05", "CLASSALPHA and ALWAYSDEFER require CLASSES");
    CREATE_FAILS(SC "ALWAYSDEFER 1 a", "CLASSALPHA and ALWAYSDEFER require CLASSES");

    /* The class target itself. */
    CREATE_FAILS(SC "CLASSES 2 a b CLASSALPHA 1 a x",
                 "CLASSALPHA for 'a' must be a finite decimal number");
    CREATE_FAILS(SC "CLASSES 2 a b CLASSALPHA 1 b nan",
                 "CLASSALPHA for 'b' must be a finite decimal number");
    CREATE_FAILS_WITH(SC "CLASSES 2 a b CLASSALPHA 1 a 0.2", "CLASSALPHA for 'a'");
    CREATE_FAILS_WITH(SC "CLASSES 2 a b CLASSALPHA 1 a 0.1000001", "CLASSALPHA for 'a'");
    CREATE_FAILS_WITH(SC "CLASSES 2 a b CLASSALPHA 1 b 0", "CLASSALPHA for 'b'");
    CREATE_FAILS_WITH(SC "CLASSES 2 a b CLASSALPHA 1 b -0.05", "CLASSALPHA for 'b'");

    /* Deferring every class leaves nothing to serve; all but one is fine. */
    CREATE_FAILS_WITH(SC "CLASSES 2 a b ALWAYSDEFER 2 a b", "ALWAYSDEFER cannot cover every class");
    CREATE_FAILS_WITH(SC "CLASSES 3 a b c ALWAYSDEFER 3 c a b",
                      "ALWAYSDEFER cannot cover every class");
    CREATE_OK_OR_RETURN(SC "CLASSES 3 a b c ALWAYSDEFER 2 a b", &cfg);
    if (cfg.class_defer != NULL)
        CHECK(cfg.class_defer[0] == 1 && cfg.class_defer[1] == 1 && cfg.class_defer[2] == 0);
    gateConfigFree(&cfg);
#undef SC
}

TEST(create_score_mode_rejects_classifier_options) {
    gateConfig cfg;

    CREATE_FAILS("MODE SCORE ALPHA 0.1 INPUTS 1 TEXT", "SCORE mode does not accept INPUTS");
    CREATE_FAILS("MODE SCORE ALPHA 0.1 TEXTDIM 8192", "SCORE mode does not accept TEXTDIM");
    CREATE_FAILS("MODE SCORE ALPHA 0.1 VECTORDIM 4", "SCORE mode does not accept VECTORDIM");
    CREATE_FAILS("MODE SCORE ALPHA 0.1 FEATUREDIM 256", "SCORE mode does not accept FEATUREDIM");
    /* Even the default values are refused: it is the option that is wrong. */
    CREATE_FAILS("MODE SCORE ALPHA 0.1 LR 0.1", "SCORE mode does not accept LR");
    CREATE_FAILS("MODE SCORE ALPHA 0.1 L2 0", "SCORE mode does not accept L2");
    /* MODE may come last. */
    CREATE_FAILS("LR 0.1 ALPHA 0.1 MODE SCORE", "SCORE mode does not accept LR");
    CREATE_FAILS("INPUTS 2 TEXT FEATURES ALPHA 0.1 MODE SCORE", "SCORE mode does not accept INPUTS");
    /* Several at once: reported in the order the README lists them. */
    CREATE_FAILS("MODE SCORE ALPHA 0.1 L2 0 LR 0.1 TEXTDIM 8", "SCORE mode does not accept TEXTDIM");

    /* Everything else is accepted by a SCORE gate. */
    CREATE_OK_OR_RETURN("MODE SCORE ALPHA 0.1 DELTA 0.02 AUDIT 0.2 COLDAUDIT 0.3 THRESHOLDS 16 "
                        "CLASSES 2 hit miss CLASSALPHA 1 hit 0.05 ALWAYSDEFER 1 miss TEACHER t "
                        "SCHEMAVERSION s MAXMEMORY 100000 MAXPENDING 100 OBSERVATIONTTL 5000 "
                        "CERTTTL 86400000 DRIFTMARGIN 0.05 ONMISSING ERROR EXPAND YES", &cfg);
    CHECK_EQ_INT(cfg.mode, GATE_MODE_SCORE);
    CHECK(isAllZero(&cfg.schema, sizeof(cfg.schema)));
    CHECK_EQ_INT(cfg.nclasses, 2);
    CHECK_EQ_INT(cfg.nthresholds, 16);
    CHECK_EQ_INT(cfg.max_memory, 100000);
    CHECK_EQ_INT(cfg.max_pending, 100);
    CHECK_EQ_INT(cfg.observation_ttl_ms, 5000);
    CHECK_EQ_INT(cfg.cert_ttl_ms, 86400000);
    CHECK(cfg.drift_margin == 0.05);
    CHECK_EQ_INT(cfg.on_missing, GATE_ONMISSING_ERROR);
    CHECK_EQ_INT(cfg.expand, 1);
    gateConfigFree(&cfg);
}

TEST(create_classifier_dimensions) {
    gateConfig cfg;

#define CLS "MODE CLASSIFIER ALPHA 0.1 CLASSES 2 a b "
    /* Defaults only for the inputs that are configured. */
    CREATE_OK_OR_RETURN(CLS "INPUTS 1 TEXT", &cfg);
    CHECK_EQ_INT(cfg.schema.inputs, GATE_INPUT_TEXT);
    CHECK(cfg.schema.text_dim == 8192 && cfg.schema.vector_dim == 0 && cfg.schema.feature_dim == 0);
    gateConfigFree(&cfg);
    CREATE_OK_OR_RETURN(CLS "INPUTS 1 FEATURES", &cfg);
    CHECK_EQ_INT(cfg.schema.inputs, GATE_INPUT_FEATURES);
    CHECK(cfg.schema.text_dim == 0 && cfg.schema.vector_dim == 0 && cfg.schema.feature_dim == 256);
    gateConfigFree(&cfg);
    CREATE_OK_OR_RETURN(CLS "INPUTS 1 VECTOR VECTORDIM 384", &cfg);
    CHECK_EQ_INT(cfg.schema.inputs, GATE_INPUT_VECTOR);
    CHECK(cfg.schema.text_dim == 0 && cfg.schema.vector_dim == 384 && cfg.schema.feature_dim == 0);
    gateConfigFree(&cfg);
    CREATE_OK_OR_RETURN(CLS "INPUTS 3 VECTOR FEATURES TEXT VECTORDIM 16", &cfg);
    CHECK_EQ_INT(cfg.schema.inputs, GATE_INPUT_TEXT | GATE_INPUT_VECTOR | GATE_INPUT_FEATURES);
    CHECK(cfg.schema.text_dim == 8192 && cfg.schema.vector_dim == 16 &&
          cfg.schema.feature_dim == 256);
    gateConfigFree(&cfg);
    /* Explicit dimensions, written before INPUTS. */
    CREATE_OK_OR_RETURN(CLS "TEXTDIM 100 FEATUREDIM 7 INPUTS 2 FEATURES TEXT", &cfg);
    CHECK(cfg.schema.text_dim == 100 && cfg.schema.vector_dim == 0 && cfg.schema.feature_dim == 7);
    gateConfigFree(&cfg);

    /* VECTOR has no default dimension. */
    CREATE_FAILS(CLS "INPUTS 1 VECTOR", "the VECTOR input requires VECTORDIM");
    CREATE_FAILS(CLS "INPUTS 3 TEXT VECTOR FEATURES TEXTDIM 64 FEATUREDIM 8",
                 "the VECTOR input requires VECTORDIM");
    /* A dimension for an input that is not configured, including one that
     * is written with the default value. */
    CREATE_FAILS(CLS "INPUTS 1 TEXT VECTORDIM 8", "VECTORDIM requires the VECTOR input");
    CREATE_FAILS(CLS "INPUTS 1 FEATURES TEXTDIM 8192", "TEXTDIM requires the TEXT input");
    CREATE_FAILS(CLS "INPUTS 1 TEXT FEATUREDIM 256", "FEATUREDIM requires the FEATURES input");
    CREATE_FAILS(CLS "TEXTDIM 64 INPUTS 2 VECTOR FEATURES VECTORDIM 4",
                 "TEXTDIM requires the TEXT input");

    /* The inputs themselves. */
    CREATE_FAILS(CLS "INPUTS 2 TEXT TEXT", "input TEXT listed more than once");
    CREATE_FAILS(CLS "INPUTS 2 text TEXT", "input TEXT listed more than once");
    CREATE_FAILS(CLS "INPUTS 3 VECTOR FEATURES vector", "input VECTOR listed more than once");
    CREATE_FAILS(CLS "INPUTS 1 IMAGE", "INPUTS accepts TEXT, VECTOR or FEATURES, not 'IMAGE'");
    CREATE_FAILS(CLS "INPUTS 2 TEXT TEXTDIM 64",
                 "INPUTS accepts TEXT, VECTOR or FEATURES, not 'TEXTDIM'");
#undef CLS

    /* What a CLASSIFIER cannot do without (validator messages). */
    CREATE_FAILS_WITH("MODE CLASSIFIER ALPHA 0.1 CLASSES 2 a b", "requires INPUTS");
    CREATE_FAILS_WITH("MODE CLASSIFIER ALPHA 0.1 CLASSES 2 a b TEXTDIM 64", "requires INPUTS");
    CREATE_FAILS_WITH("MODE CLASSIFIER ALPHA 0.1 CLASSES 2 a b VECTORDIM 4", "requires INPUTS");
    CREATE_FAILS_WITH("MODE CLASSIFIER ALPHA 0.1 INPUTS 1 TEXT", "requires CLASSES");
    CREATE_FAILS_WITH("MODE CLASSIFIER ALPHA 0.1", "requires CLASSES");
}

TEST(create_maxmemory_must_hold_the_model) {
    gateConfig cfg;
    char line[256], number[32];
    uint64_t need;

#define CLS "MODE CLASSIFIER ALPHA 0.1 CLASSES 2 a b INPUTS 1 TEXT"
    CREATE_OK_OR_RETURN(CLS, &cfg);
    need = gateFixedBytes(&cfg); /* gate_config.c's definition of the footprint */
    gateConfigFree(&cfg);
    /* An independent floor: the trainer, a candidate and a serving copy of
     * 8192 float32 weights. And a ceiling that rules out a nonsense value. */
    CHECK(need >= 3ull * 8192 * 4);
    CHECK(need < 1000000);
    snprintf(number, sizeof(number), "%llu", (unsigned long long)need);

    /* Exactly enough is accepted, one byte less is not, and the message
     * says how many bytes are needed. */
    snprintf(line, sizeof(line), CLS " MAXMEMORY %llu", (unsigned long long)need);
    CREATE_OK_OR_RETURN(line, &cfg);
    CHECK_EQ_INT(cfg.max_memory, need);
    gateConfigFree(&cfg);

    snprintf(line, sizeof(line), CLS " MAXMEMORY %llu", (unsigned long long)(need - 1));
    CREATE_FAILS_WITH(line, "MAXMEMORY");
    CHECK(errorHas(number));
    CREATE_FAILS_WITH(CLS " MAXMEMORY 1000", number);
    /* 0 is not "unlimited". */
    CREATE_FAILS_WITH(CLS " MAXMEMORY 0", number);
    /* Wherever MAXMEMORY is written, the check sees the final model. */
    CREATE_FAILS_WITH("MAXMEMORY 1000 " CLS, number);
#undef CLS

    /* A SCORE gate has no model: it needs less, with the same sharp edge. */
    {
        uint64_t score_need;

        CREATE_OK_OR_RETURN("MODE SCORE ALPHA 0.1", &cfg);
        score_need = gateFixedBytes(&cfg);
        gateConfigFree(&cfg);
        CHECK(score_need > 0 && score_need + 3ull * 8192 * 4 <= need);
        snprintf(line, sizeof(line), "MODE SCORE ALPHA 0.1 MAXMEMORY %llu",
                 (unsigned long long)score_need);
        CREATE_OK_OR_RETURN(line, &cfg);
        CHECK_EQ_INT(cfg.max_memory, score_need);
        gateConfigFree(&cfg);
        snprintf(number, sizeof(number), "%llu", (unsigned long long)score_need);
        snprintf(line, sizeof(line), "MODE SCORE ALPHA 0.1 MAXMEMORY %llu",
                 (unsigned long long)(score_need - 1));
        CREATE_FAILS_WITH(line, number);
    }
}

/* Byte strings that the gate's name policy refuses. */
static const struct {
    const char *bytes;
    size_t len;
} g_bad_names[] = {
    {LIT("")},                 /* empty */
    {LIT("\xff")},             /* not UTF-8 at all */
    {LIT("ab\xfe")},
    {LIT("\x80")},             /* lone continuation byte */
    {LIT("\xc3")},             /* truncated two byte sequence */
    {LIT("\xe2\x82")},         /* truncated three byte sequence */
    {LIT("\xc3\x28")},         /* bad continuation byte */
    {LIT("\xc0\x80")},         /* overlong NUL */
    {LIT("\xc1\xbf")},         /* overlong DEL */
    {LIT("\xe0\x80\xaf")},     /* overlong '/' */
    {LIT("\xed\xa0\x80")},     /* UTF-16 surrogate U+D800 */
    {LIT("\xf4\x90\x80\x80")}, /* above U+10FFFF */
    {LIT("a\nb")},             /* C0 controls */
    {LIT("a\tb")},
    {LIT("a\rb")},
    {LIT("\x1b[31m")},
    {LIT("a\0b")},             /* embedded NUL */
    {LIT("\x7f")},             /* DEL */
    {LIT("\xc2\x80")},         /* C1 controls */
    {LIT("x\xc2\x9fy")},
};
#define BAD_NAME_COUNT (sizeof(g_bad_names) / sizeof(g_bad_names[0]))

TEST(create_names_follow_the_utf8_policy) {
    const char *label_msg = "class labels must be 1 to 64 bytes of UTF-8 without control characters";
    gateConfig cfg;
    testArgs a;
    char big[200];
    size_t i;
    int rc;

    memset(big, 'a', sizeof(big));
    argsInit(&a);
    for (i = 0; i < BAD_NAME_COUNT; i++) {
        /* As the first and as the last label. */
        argsSplit(&a, "MODE SCORE ALPHA 0.1 CLASSES 2");
        argsPushLen(&a, g_bad_names[i].bytes, g_bad_names[i].len);
        argsPush(&a, "ok");
        CREATE_ARGS_FAIL(&a, label_msg);
        argsFree(&a);
        argsSplit(&a, "MODE SCORE ALPHA 0.1 CLASSES 2 ok");
        argsPushLen(&a, g_bad_names[i].bytes, g_bad_names[i].len);
        CREATE_ARGS_FAIL(&a, label_msg);
        argsFree(&a);

        argsSplit(&a, "MODE SCORE ALPHA 0.1 TEACHER");
        argsPushLen(&a, g_bad_names[i].bytes, g_bad_names[i].len);
        CREATE_ARGS_FAIL(&a, "TEACHER must be 1 to 128 bytes of UTF-8 without control characters");
        argsFree(&a);
        argsSplit(&a, "MODE SCORE ALPHA 0.1 SCHEMAVERSION");
        argsPushLen(&a, g_bad_names[i].bytes, g_bad_names[i].len);
        CREATE_ARGS_FAIL(&a, "SCHEMAVERSION must be 1 to 128 bytes of UTF-8 without control "
                             "characters");
        argsFree(&a);
    }

    /* Lengths: 64 bytes of label and 128 of version are the limits. */
    argsSplit(&a, "MODE SCORE ALPHA 0.1 CLASSES 2 ok");
    argsPushLen(&a, big, 65);
    CREATE_ARGS_FAIL(&a, label_msg);
    argsFree(&a);
    argsSplit(&a, "MODE SCORE ALPHA 0.1 TEACHER");
    argsPushLen(&a, big, 129);
    CREATE_ARGS_FAIL(&a, "TEACHER must be 1 to 128 bytes of UTF-8 without control characters");
    argsFree(&a);
    argsSplit(&a, "MODE SCORE ALPHA 0.1 SCHEMAVERSION");
    argsPushLen(&a, big, 129);
    CREATE_ARGS_FAIL(&a, "SCHEMAVERSION must be 1 to 128 bytes of UTF-8 without control "
                         "characters");
    argsFree(&a);

    /* What the policy accepts: the limits, spaces, and real UTF-8. */
    argsSplit(&a, "MODE SCORE ALPHA 0.1 CLASSES 4");
    argsPushLen(&a, big, 64);
    argsPush(&a, "caf\xc3\xa9");                          /* U+00E9 */
    argsPush(&a, "\xe6\x97\xa5\xe6\x9c\xac \xf0\x9f\x98\x80"); /* CJK, a space, U+1F600 */
    argsPush(&a, "two words");
    argsPush(&a, "TEACHER");
    argsPushLen(&a, big, 128);
    argsPush(&a, "SCHEMAVERSION");
    argsPush(&a, "sch\xc3\xa9ma v2");
    rc = createArgs(&a, &cfg);
    argsFree(&a);
    CHECK_EQ_INT(rc, GATE_OK);
    if (rc != GATE_OK) return;
    CHECK(cfg.labels != NULL && strlen(cfg.labels[0]) == 64 && memcmp(cfg.labels[0], big, 64) == 0);
    CHECK(labelIs(&cfg, 1, "caf\xc3\xa9"));
    CHECK(labelIs(&cfg, 2, "\xe6\x97\xa5\xe6\x9c\xac \xf0\x9f\x98\x80"));
    CHECK(labelIs(&cfg, 3, "two words"));
    CHECK(strlen(cfg.teacher) == 128 && memcmp(cfg.teacher, big, 128) == 0);
    CHECK_EQ_STR(cfg.schema_version, "sch\xc3\xa9ma v2");
    gateConfigFree(&cfg);

    /* Distinct means distinct bytes. */
    CREATE_FAILS("MODE SCORE ALPHA 0.1 CLASSES 2 a a", "duplicate class label 'a'");
    CREATE_FAILS("MODE SCORE ALPHA 0.1 CLASSES 3 a b a", "duplicate class label 'a'");
    CREATE_FAILS("MODE SCORE ALPHA 0.1 CLASSES 4 a bb c bb", "duplicate class label 'bb'");
    argsSplit(&a, "MODE SCORE ALPHA 0.1 CLASSES 2 caf\xc3\xa9 caf\xc3\xa9");
    CREATE_ARGS_FAIL(&a, "duplicate class label 'caf\xc3\xa9'");
    argsFree(&a);
    /* A 64 byte label is quoted in full. */
    argsSplit(&a, "MODE SCORE ALPHA 0.1 CLASSES 2");
    argsPushLen(&a, big, 64);
    argsPushLen(&a, big, 64);
    CHECK(createArgsRefused(&a, &rc));
    CHECK(strlen(g_err) == strlen("duplicate class label ''") + 64);
    CHECK(strstr(g_err, "...") == NULL);
    argsFree(&a);
    /* A prefix is a different label. */
    CREATE_OK_OR_RETURN("MODE SCORE ALPHA 0.1 CLASSES 3 a aa aaa", &cfg);
    CHECK_EQ_INT(cfg.nclasses, 3);
    gateConfigFree(&cfg);
}

TEST(create_messages_are_one_printable_line) {
    testArgs a;
    char big[300];
    size_t i;

    /* Whatever the client sends, the echo in the message is harmless. */
    argsInit(&a);
    argsPushLen(&a, LIT("FOO\r\nBAR"));
    CREATE_ARGS_FAIL(&a, "unknown option 'FOO??BAR'");
    argsFree(&a);
    argsPushLen(&a, LIT("\0\x01\x1f\x7f"));
    CREATE_ARGS_FAIL(&a, "unknown option '????" "'"); /* no ??' trigraph */
    argsFree(&a);
    argsPushLen(&a, LIT("\xff\xfe"));
    CREATE_ARGS_FAIL(&a, "unknown option '??" "'");
    argsFree(&a);
    /* A stray lead byte, a C1 control and a surrogate are replaced; valid
     * UTF-8 around them is kept. */
    argsPushLen(&a, LIT("a\xc3" "b\xc2\x80" "c\xed\xa0\x80" "d\xc3\xa9"));
    CREATE_ARGS_FAIL(&a, "unknown option 'a?b??c???d\xc3\xa9'");
    argsFree(&a);
    argsPushLen(&a, LIT("\xe6\x97\xa5\xe6\x9c\xac"));
    CREATE_ARGS_FAIL(&a, "unknown option '\xe6\x97\xa5\xe6\x9c\xac'");
    argsFree(&a);
    argsPushLen(&a, LIT("truncated\xe6\x97"));
    CREATE_ARGS_FAIL(&a, "unknown option 'truncated??" "'");
    argsFree(&a);

    /* Long arguments are cut at 64 bytes and marked. */
    memset(big, 'x', sizeof(big));
    argsPushLen(&a, big, 64);
    CHECK(createArgsRefused(&a, (int[]){0}));
    CHECK(strlen(g_err) == strlen("unknown option ''") + 64 && strstr(g_err, "...") == NULL);
    argsFree(&a);
    argsPushLen(&a, big, 65);
    CHECK(createArgsRefused(&a, (int[]){0}));
    CHECK(strlen(g_err) == strlen("unknown option '...'") + 64);
    CHECK(strstr(g_err, "x...'") != NULL);
    argsFree(&a);
    argsPushLen(&a, big, 300);
    CHECK(createArgsRefused(&a, (int[]){0}));
    CHECK(strlen(g_err) == strlen("unknown option '...'") + 64);
    argsFree(&a);
    /* The cut never splits a multi-byte character: 63 bytes of ASCII and a
     * two byte character do not fit 64, 62 and the character do. */
    memcpy(big + 63, "\xc3\xa9", 2);
    argsPushLen(&a, big, 65);
    CHECK(createArgsRefused(&a, (int[]){0}));
    CHECK(strlen(g_err) == strlen("unknown option '...'") + 63);
    CHECK(strstr(g_err, "\xc3") == NULL);
    argsFree(&a);
    memset(big, 'x', sizeof(big));
    memcpy(big + 62, "\xc3\xa9", 2);
    argsPushLen(&a, big, 64);
    CHECK(createArgsRefused(&a, (int[]){0}));
    CHECK(strlen(g_err) == strlen("unknown option ''") + 64);
    CHECK(strstr(g_err, "x\xc3\xa9'") != NULL);
    argsFree(&a);

    /* Control bytes in a class label of a class option. */
    argsSplit(&a, "MODE SCORE ALPHA 0.1 CLASSES 2 a b ALWAYSDEFER 1");
    argsPushLen(&a, LIT("a\nb"));
    CREATE_ARGS_FAIL(&a, "unknown class label 'a?b'");
    argsFree(&a);

    /* No message of the cases above carried a control byte. */
    argsPushLen(&a, LIT("\x01\x02\r\n\x7f"));
    CHECK(createArgsRefused(&a, (int[]){0}));
    for (i = 0; g_err[i] != '\0'; i++) CHECK((unsigned char)g_err[i] >= 0x20 && g_err[i] != 0x7f);
    argsFree(&a);
}

TEST(create_error_buffer_edge_cases) {
    gateConfig cfg;
    testArgs a;
    char small[16];

    argsInit(&a);
    argsSplit(&a, "MODE SCORE ALPHA 0.1 FOO");

    /* No buffer at all. */
    memset(&cfg, 0xa5, sizeof(cfg));
    CHECK_EQ_INT(gateParseCreate(a.v, a.n, &cfg, NULL, 0), GATE_ERR);
    CHECK(configIsZero(&cfg));
    memset(&cfg, 0xa5, sizeof(cfg));
    CHECK_EQ_INT(gateParseCreate(a.v, a.n, &cfg, NULL, 100), GATE_ERR);
    CHECK(configIsZero(&cfg));

    /* A buffer of zero bytes is not written. */
    memset(small, 'z', sizeof(small));
    memset(&cfg, 0xa5, sizeof(cfg));
    CHECK_EQ_INT(gateParseCreate(a.v, a.n, &cfg, small, 0), GATE_ERR);
    CHECK(small[0] == 'z');

    /* A short one gets a terminated prefix and nothing past its end. */
    memset(small, 'z', sizeof(small));
    memset(&cfg, 0xa5, sizeof(cfg));
    CHECK_EQ_INT(gateParseCreate(a.v, a.n, &cfg, small, 8), GATE_ERR);
    CHECK_EQ_STR(small, "unknown");
    CHECK(small[8] == 'z');
    memset(small, 'z', sizeof(small));
    CHECK_EQ_INT(gateParseCreate(a.v, a.n, &cfg, small, 1), GATE_ERR);
    CHECK(small[0] == '\0' && small[1] == 'z');
    argsFree(&a);

    /* The validator's message goes through the same buffer. */
    argsSplit(&a, "MODE SCORE ALPHA 7");
    memset(small, 'z', sizeof(small));
    CHECK_EQ_INT(gateParseCreate(a.v, a.n, &cfg, small, 6), GATE_ERR);
    CHECK_EQ_STR(small, "ALPHA");
    CHECK(small[6] == 'z');
    CHECK(configIsZero(&cfg));
    argsFree(&a);

    /* No arguments at all, passed as a NULL vector. */
    memset(&cfg, 0xa5, sizeof(cfg));
    poisonError();
    CHECK_EQ_INT(gateParseCreate(NULL, 0, &cfg, g_err, sizeof(g_err)), GATE_ERR);
    CHECK_EQ_STR(g_err, "missing required option MODE");
    CHECK(configIsZero(&cfg));
}

TEST(create_failure_releases_everything) {
    /* One failure per stage of the parse, each after heap fields of the
     * config were already built. */
    static const char *const lines[] = {
        /* in the walk, after labels and a version */
        "MODE CLASSIFIER CLASSES 2 a b TEACHER t ALPHA x",
        /* in the walk, in the middle of the label list */
        "MODE SCORE ALPHA 0.1 TEACHER t CLASSES 3 a b a",
        /* mode rules */
        "MODE SCORE ALPHA 0.1 CLASSES 2 a b TEACHER t SCHEMAVERSION s LR 0.1",
        /* class policy, after its arrays were allocated */
        "MODE SCORE ALPHA 0.1 CLASSES 2 a b TEACHER t CLASSALPHA 1 zzz 0.05",
        "MODE SCORE ALPHA 0.1 CLASSES 2 a b ALWAYSDEFER 1 a CLASSALPHA 1 a 0.05",
        /* the validator, with everything built */
        "MODE SCORE ALPHA 0.1 CLASSES 2 a b TEACHER t SCHEMAVERSION s ALWAYSDEFER 2 a b",
        "MODE CLASSIFIER ALPHA 0.1 CLASSES 2 a b INPUTS 1 TEXT CLASSALPHA 1 a 0.05 MAXMEMORY 10",
    };
    size_t i;

    for (i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        int zeroed = 0;

        CHECK(createRefused(lines[i], &zeroed));
        CHECK(zeroed);
        CHECK_EQ_INT(g_alloc_live, 0);
    }
}

/* Fail the 1st, 2nd, ... allocation of a parse in turn until it goes
 * through. Returns how many allocations the successful parse made, or -1 if
 * a failure was not reported cleanly. */
static int createAllocationSweep(const char *line) {
    int fail_at;

    for (fail_at = 1; fail_at < 200; fail_at++) {
        gateConfig cfg;
        int rc;

        g_alloc_calls = 0;
        g_alloc_fail_at = fail_at;
        rc = createLine(line, &cfg);
        g_alloc_fail_at = 0;
        if (rc == GATE_OK) {
            gateConfigFree(&cfg);
            return g_alloc_live == 0 ? fail_at - 1 : -1;
        }
        if (strcmp(g_err, "out of memory") != 0 || !configIsZero(&cfg) || g_alloc_live != 0) {
            printf("    allocation %d failing: \"%s\", %ld live blocks\n", fail_at, g_err,
                   g_alloc_live);
            return -1;
        }
    }
    return -1;
}

TEST(create_survives_every_allocation_failure) {
    /* The expected counts follow from how gateConfigFree() releases a
     * config: the label array, one block per label, the two versions, and
     * the two per-class arrays. */
    CHECK_EQ_INT(createAllocationSweep("MODE SCORE ALPHA 0.1"), 2);
    CHECK_EQ_INT(createAllocationSweep("MODE SCORE ALPHA 0.1 TEACHER t SCHEMAVERSION s"), 2);
    CHECK_EQ_INT(createAllocationSweep("MODE SCORE ALPHA 0.1 CLASSES 2 a b"), 1 + 2 + 2);
    CHECK_EQ_INT(createAllocationSweep("MODE CLASSIFIER ALPHA 0.1 CLASSES 3 a b c INPUTS 1 TEXT "
                                       "CLASSALPHA 1 a 0.05 ALWAYSDEFER 1 b TEACHER t"),
                 1 + 3 + 2 + 2);
    CHECK_EQ_INT(createAllocationSweep("SCHEMAVERSION s ALWAYSDEFER 1 b MODE SCORE "
                                       "CLASSES 4 a b c d ALPHA 0.1"),
                 1 + 4 + 2 + 2);
}

/* ============================ Request sections ============================ */

static int requestIsZero(const gateRequest *req) {
    return isAllZero(req, sizeof(*req));
}

/* 'req' is zeroed first, as the contract asks of callers. */
static int sectionsArgs(const testArgs *a, int *pos, gateRequest *req) {
    memset(req, 0, sizeof(*req));
    poisonError();
    return gateParseSections(a->v, a->n, pos, req, g_err, sizeof(g_err));
}

/* 1 if the line is refused, leaving an empty request and '*pos' untouched. */
static int sectionsArgsRefused(const testArgs *a) {
    gateRequest req;
    int pos = 0, rc = sectionsArgs(a, &pos, &req);
    int clean = requestIsZero(&req) && pos == 0 && g_alloc_live == 0;

    if (rc == GATE_OK) gateRequestFree(&req);
    if (rc != GATE_ERR) printf("    the sections were accepted\n");
    else if (!clean) printf("    the request was not left empty (pos %d)\n", pos);
    return rc == GATE_ERR && clean;
}

static int sectionsRefused(const char *line) {
    testArgs a;
    int refused;

    argsInit(&a);
    argsSplit(&a, line);
    refused = sectionsArgsRefused(&a);
    argsFree(&a);
    return refused;
}

#define SECTIONS_FAIL(line, msg) do { \
    CHECK(sectionsRefused(line)); \
    CHECK_EQ_STR(g_err, (msg)); \
} while (0)

#define SECTIONS_ARGS_FAIL(args, msg) do { \
    CHECK(sectionsArgsRefused(args)); \
    CHECK_EQ_STR(g_err, (msg)); \
} while (0)

TEST(sections_in_every_order_and_subset) {
    static const int perms[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    /* Binary on purpose: spaces, NULs, the bytes of a section token. */
    static const char blob[] = {0x00, ' ', (char)0xff, 'T', 'E', 'X', 'T', 0x00, '\n', (char)0x80,
                                0x00, 0x00};
    static const uint32_t bits[3] = {GATE_INPUT_TEXT, GATE_INPUT_VECTOR, GATE_INPUT_FEATURES};
    int p, mask;

    for (p = 0; p < 6; p++) {
        for (mask = 0; mask < 8; mask++) {
            gateRequest req;
            testArgs a;
            int text_at = -1, vector_at = -1, features_at = -1, pos = 2, k, rc;

            /* Two arguments the parser must not look at, then the sections
             * of 'mask' in the order of the permutation, then the caller's
             * own grammar. */
            argsInit(&a);
            argsSplit(&a, "GADD TEXT");
            for (k = 0; k < 3; k++) {
                int s = perms[p][k];

                if (!(mask & (1 << s))) continue;
                if (s == 0) {
                    argsPush(&a, "TEXT");
                    text_at = a.n;
                    argsPush(&a, "Please explain this");
                } else if (s == 1) {
                    argsPush(&a, "VECTOR");
                    vector_at = a.n;
                    argsPushLen(&a, blob, sizeof(blob));
                } else {
                    argsPush(&a, "FEATURES");
                    argsPush(&a, "2");
                    features_at = a.n;
                    argsSplit(&a, "mentioned 1 previous_speaker_agent -2.5");
                }
            }
            argsSplit(&a, "LABEL respond");

            rc = sectionsArgs(&a, &pos, &req);
            CHECK_EQ_INT(rc, GATE_OK);
            if (rc != GATE_OK) {
                argsFree(&a);
                continue;
            }
            /* Stopped at LABEL, having consumed every section. */
            CHECK_EQ_INT(pos, a.n - 2);
            CHECK_EQ_INT(req.in.present, (mask & 1 ? bits[0] : 0) | (mask & 2 ? bits[1] : 0) |
                                         (mask & 4 ? bits[2] : 0));
            if (text_at >= 0) {
                /* Borrowed from the argument vector, not copied. */
                CHECK(req.in.text == a.v[text_at].ptr);
                CHECK_EQ_INT(req.in.text_len, 19);
            } else {
                CHECK(req.in.text == NULL && req.in.text_len == 0);
            }
            if (vector_at >= 0) {
                CHECK(req.in.vector == (const unsigned char *)a.v[vector_at].ptr);
                CHECK_EQ_INT(req.in.vector_len, 12);
                CHECK(memcmp(req.in.vector, blob, sizeof(blob)) == 0);
            } else {
                CHECK(req.in.vector == NULL && req.in.vector_len == 0);
            }
            if (features_at >= 0) {
                CHECK_EQ_INT(req.in.nfeatures, 2);
                CHECK(req.features != NULL && req.in.features == req.features);
                if (req.features != NULL && req.in.nfeatures == 2) {
                    CHECK(req.features[0].name == a.v[features_at].ptr);
                    CHECK_EQ_INT(req.features[0].name_len, 9);
                    CHECK(req.features[0].value == 1.0);
                    CHECK(req.features[1].name == a.v[features_at + 2].ptr);
                    CHECK_EQ_INT(req.features[1].name_len, 22);
                    CHECK(req.features[1].value == -2.5);
                }
            } else {
                CHECK(req.features == NULL && req.in.features == NULL && req.in.nfeatures == 0);
            }
            gateRequestFree(&req);
            CHECK(requestIsZero(&req));
            argsFree(&a);
        }
    }
}

TEST(sections_stop_at_the_first_foreign_token) {
    gateRequest req;
    testArgs a;
    int pos;

    /* Nothing to parse: no arguments, or a foreign token right away. */
    argsInit(&a);
    pos = 0;
    CHECK_EQ_INT(sectionsArgs(&a, &pos, &req), GATE_OK);
    CHECK(pos == 0 && requestIsZero(&req));
    argsSplit(&a, "LABEL x TEXT hi");
    pos = 0;
    CHECK_EQ_INT(sectionsArgs(&a, &pos, &req), GATE_OK);
    CHECK(pos == 0 && requestIsZero(&req));
    /* Starting at the end of the vector. */
    pos = a.n;
    CHECK_EQ_INT(sectionsArgs(&a, &pos, &req), GATE_OK);
    CHECK(pos == a.n && requestIsZero(&req));
    /* Starting in the middle of it. */
    pos = 2;
    CHECK_EQ_INT(sectionsArgs(&a, &pos, &req), GATE_OK);
    CHECK_EQ_INT(pos, 4);
    CHECK_EQ_INT(req.in.present, GATE_INPUT_TEXT);
    CHECK(req.in.text == a.v[3].ptr && req.in.text_len == 2);
    gateRequestFree(&req);
    argsFree(&a);

    /* Tokens are case-insensitive, like option names. */
    argsSplit(&a, "text hi Vector vv fEATURES 1 a 1 probs 0.5 0.5");
    pos = 0;
    CHECK_EQ_INT(sectionsArgs(&a, &pos, &req), GATE_OK);
    CHECK_EQ_INT(pos, 8);
    CHECK_EQ_INT(req.in.present, GATE_INPUT_TEXT | GATE_INPUT_VECTOR | GATE_INPUT_FEATURES);
    gateRequestFree(&req);
    argsFree(&a);

    /* Values are positional: a value that spells a token is a value. */
    argsSplit(&a, "TEXT VECTOR LABEL x");
    pos = 0;
    CHECK_EQ_INT(sectionsArgs(&a, &pos, &req), GATE_OK);
    CHECK_EQ_INT(pos, 2);
    CHECK_EQ_INT(req.in.present, GATE_INPUT_TEXT);
    CHECK(req.in.text == a.v[1].ptr && req.in.text_len == 6);
    gateRequestFree(&req);
    argsFree(&a);
    argsSplit(&a, "VECTOR FEATURES FEATURES 1 TEXT 3 TEXT TEXT");
    pos = 0;
    CHECK_EQ_INT(sectionsArgs(&a, &pos, &req), GATE_OK);
    CHECK_EQ_INT(pos, 8);
    CHECK_EQ_INT(req.in.present, GATE_INPUT_TEXT | GATE_INPUT_VECTOR | GATE_INPUT_FEATURES);
    CHECK(req.in.vector == (const unsigned char *)a.v[1].ptr && req.in.vector_len == 8);
    CHECK_EQ_INT(req.in.nfeatures, 1);
    if (req.features != NULL && req.in.nfeatures == 1) {
        CHECK(req.features[0].name == a.v[4].ptr && req.features[0].name_len == 4);
        CHECK(req.features[0].value == 3.0);
    }
    CHECK(req.in.text == a.v[7].ptr && req.in.text_len == 4);
    gateRequestFree(&req);
    argsFree(&a);

    /* Something that only resembles a token ends the sections. */
    argsSplit(&a, "TEXT hi TEXTS more");
    pos = 0;
    CHECK_EQ_INT(sectionsArgs(&a, &pos, &req), GATE_OK);
    CHECK_EQ_INT(pos, 2);
    gateRequestFree(&req);
    argsFree(&a);
}

TEST(sections_reject_repeats_and_truncation) {
    SECTIONS_FAIL("TEXT a TEXT b", "section TEXT given more than once");
    SECTIONS_FAIL("TEXT a text b", "section TEXT given more than once");
    SECTIONS_FAIL("VECTOR a VECTOR b", "section VECTOR given more than once");
    SECTIONS_FAIL("FEATURES 0 FEATURES 0", "section FEATURES given more than once");
    SECTIONS_FAIL("TEXT a VECTOR v FEATURES 1 x 1 TEXT b", "section TEXT given more than once");
    SECTIONS_FAIL("FEATURES 1 x 1 TEXT a VECTOR v FEATURES 1 y 2",
                  "section FEATURES given more than once");
    /* The repetition is reported even when it has no value. */
    SECTIONS_FAIL("VECTOR v TEXT a VECTOR", "section VECTOR given more than once");
    /* First TEXT is the token, second its value, third a repetition. */
    SECTIONS_FAIL("TEXT TEXT TEXT", "section TEXT given more than once");

    SECTIONS_FAIL("TEXT", "section TEXT requires 1 argument(s)");
    SECTIONS_FAIL("VECTOR", "section VECTOR requires 1 argument(s)");
    SECTIONS_FAIL("FEATURES", "section FEATURES requires 1 argument(s)");
    SECTIONS_FAIL("TEXT hi VECTOR", "section VECTOR requires 1 argument(s)");
    SECTIONS_FAIL("FEATURES 1 a 1 text", "section TEXT requires 1 argument(s)");
}

TEST(sections_features_counts) {
    gateRequest req;
    testArgs a;
    int pos;

    /* An empty section is present. */
    argsInit(&a);
    argsSplit(&a, "FEATURES 0");
    pos = 0;
    CHECK_EQ_INT(sectionsArgs(&a, &pos, &req), GATE_OK);
    CHECK_EQ_INT(pos, 2);
    CHECK_EQ_INT(req.in.present, GATE_INPUT_FEATURES);
    CHECK_EQ_INT(req.in.nfeatures, 0);
    /* Zero pairs still come with an array a consumer can pass around. */
    CHECK(req.features != NULL && req.in.features == req.features);
    gateRequestFree(&req);
    argsFree(&a);

    argsSplit(&a, "FEATURES 0 LABEL x");
    pos = 0;
    CHECK_EQ_INT(sectionsArgs(&a, &pos, &req), GATE_OK);
    CHECK_EQ_INT(pos, 2);
    CHECK_EQ_INT(req.in.nfeatures, 0);
    gateRequestFree(&req);
    argsFree(&a);

    /* The count is the number of PAIRS. */
    argsSplit(&a, "FEATURES 2 a 1 b 2");
    pos = 0;
    CHECK_EQ_INT(sectionsArgs(&a, &pos, &req), GATE_OK);
    CHECK_EQ_INT(pos, 6);
    CHECK_EQ_INT(req.in.nfeatures, 2);
    gateRequestFree(&req);
    argsFree(&a);
    argsSplit(&a, "FEATURES 00002 a 1 b 2");
    pos = 0;
    CHECK_EQ_INT(sectionsArgs(&a, &pos, &req), GATE_OK);
    CHECK_EQ_INT(req.in.nfeatures, 2);
    gateRequestFree(&req);
    argsFree(&a);

    /* More pairs than announced: the rest is the caller's to judge. */
    argsSplit(&a, "FEATURES 1 a 1 b 2");
    pos = 0;
    CHECK_EQ_INT(sectionsArgs(&a, &pos, &req), GATE_OK);
    CHECK_EQ_INT(pos, 4);
    CHECK_EQ_INT(req.in.nfeatures, 1);
    gateRequestFree(&req);
    argsFree(&a);
    argsSplit(&a, "FEATURES 0 a 1");
    pos = 0;
    CHECK_EQ_INT(sectionsArgs(&a, &pos, &req), GATE_OK);
    CHECK_EQ_INT(pos, 2);
    gateRequestFree(&req);
    argsFree(&a);

    /* Fewer than announced. */
    SECTIONS_FAIL("FEATURES 1", "FEATURES count 1 needs 2 argument(s) after it but only 0 follow");
    SECTIONS_FAIL("FEATURES 1 a", "FEATURES count 1 needs 2 argument(s) after it but only 1 follow");
    SECTIONS_FAIL("FEATURES 2 a 1", "FEATURES count 2 needs 4 argument(s) after it but only 2 follow");
    SECTIONS_FAIL("FEATURES 2 a 1 b", "FEATURES count 2 needs 4 argument(s) after it but only 3 follow");
    SECTIONS_FAIL("TEXT hi FEATURES 3 a 1 b 2",
                  "FEATURES count 3 needs 6 argument(s) after it but only 4 follow");
    /* A count that is one too large takes the caller's tokens as a pair. */
    SECTIONS_FAIL("FEATURES 2 a 1 LABEL x", "value of feature 'LABEL' must be a finite decimal number");

    SECTIONS_FAIL("FEATURES x a 1", "FEATURES count must be an unsigned integer");
    SECTIONS_FAIL("FEATURES -1 a 1", "FEATURES count must be an unsigned integer");
    SECTIONS_FAIL("FEATURES 1.0 a 1", "FEATURES count must be an unsigned integer");
    SECTIONS_FAIL("FEATURES a 1", "FEATURES count must be an unsigned integer");
    SECTIONS_FAIL("FEATURES 1025 a 1", "FEATURES count is out of range: must be at most 1024");
    SECTIONS_FAIL("FEATURES 4294967297 a 1", "FEATURES count is out of range: must be at most 1024");
    SECTIONS_FAIL("FEATURES 18446744073709551616 a 1",
                  "FEATURES count is out of range: must be at most 1024");
}

TEST(sections_feature_values_and_names) {
    static const char *const not_numbers[] = {"abc", "nan", "inf", "-inf", "0x10", "1e999",
                                              "1,5", "--1", "1e"};
    static const char *const too_large[] = {"1000001", "-1000001", "1000000.0000001", "1e7",
                                            "-1e7", "1e300", "-1.7976931348623157e308"};
    gateRequest req;
    testArgs a;
    char line[128], big[200];
    size_t i;
    int pos;

    argsInit(&a);
    for (i = 0; i < sizeof(not_numbers) / sizeof(not_numbers[0]); i++) {
        snprintf(line, sizeof(line), "FEATURES 2 ok 1 x %s", not_numbers[i]);
        SECTIONS_FAIL(line, "value of feature 'x' must be a finite decimal number");
    }
    argsSplit(&a, "FEATURES 1 x");
    argsPushLen(&a, "", 0);
    SECTIONS_ARGS_FAIL(&a, "value of feature 'x' must be a finite decimal number");
    argsFree(&a);
    argsSplit(&a, "FEATURES 1 x");
    argsPush(&a, " 1");
    SECTIONS_ARGS_FAIL(&a, "value of feature 'x' must be a finite decimal number");
    argsFree(&a);
    argsSplit(&a, "FEATURES 1 x");
    argsPushLen(&a, LIT("1\0"));
    SECTIONS_ARGS_FAIL(&a, "value of feature 'x' must be a finite decimal number");
    argsFree(&a);

    for (i = 0; i < sizeof(too_large) / sizeof(too_large[0]); i++) {
        snprintf(line, sizeof(line), "FEATURES 2 ok 1 big %s", too_large[i]);
        SECTIONS_FAIL(line, "value of feature 'big' is out of range: must be between "
                            "-1e+06 and 1e+06");
    }

    /* The bound itself is allowed, on both sides. */
    argsSplit(&a, "FEATURES 5 a 1000000 b -1000000 c 1e6 d 0 e -0.000001");
    pos = 0;
    CHECK_EQ_INT(sectionsArgs(&a, &pos, &req), GATE_OK);
    CHECK_EQ_INT(req.in.nfeatures, 5);
    if (req.features != NULL && req.in.nfeatures == 5) {
        CHECK(req.features[0].value == 1000000.0);
        CHECK(req.features[1].value == -1000000.0);
        CHECK(req.features[2].value == 1000000.0);
        CHECK(req.features[3].value == 0.0);
        CHECK(req.features[4].value == -0.000001);
    }
    gateRequestFree(&req);
    argsFree(&a);

    /* Names follow the gate's name policy, up to 128 bytes. */
    for (i = 0; i < BAD_NAME_COUNT; i++) {
        argsSplit(&a, "FEATURES 2 ok 1");
        argsPushLen(&a, g_bad_names[i].bytes, g_bad_names[i].len);
        argsPush(&a, "1");
        SECTIONS_ARGS_FAIL(&a, "feature names must be 1 to 128 bytes of UTF-8 without control "
                               "characters");
        argsFree(&a);
    }
    memset(big, 'n', sizeof(big));
    argsSplit(&a, "FEATURES 1");
    argsPushLen(&a, big, 129);
    argsPush(&a, "1");
    SECTIONS_ARGS_FAIL(&a, "feature names must be 1 to 128 bytes of UTF-8 without control "
                           "characters");
    argsFree(&a);

    argsSplit(&a, "FEATURES 4");
    argsPushLen(&a, big, 128);
    argsPush(&a, "0.5");
    argsPush(&a, "caf\xc3\xa9");
    argsPush(&a, "1");
    argsPush(&a, "TEXT"); /* a name is a name, even if it spells a token */
    argsPush(&a, "2");
    argsPush(&a, "two words");
    argsPush(&a, "3");
    pos = 0;
    CHECK_EQ_INT(sectionsArgs(&a, &pos, &req), GATE_OK);
    CHECK_EQ_INT(pos, 10);
    CHECK_EQ_INT(req.in.nfeatures, 4);
    if (req.features != NULL && req.in.nfeatures == 4) {
        CHECK(req.features[0].name_len == 128 && req.features[0].value == 0.5);
        CHECK(req.features[1].name_len == 5 && memcmp(req.features[1].name, "caf\xc3\xa9", 5) == 0);
        CHECK(req.features[2].name_len == 4 && memcmp(req.features[2].name, "TEXT", 4) == 0);
        CHECK(req.features[3].name_len == 9 && req.features[3].value == 3.0);
    }
    gateRequestFree(&req);
    argsFree(&a);

    /* A long name is abbreviated in the message about its value. */
    argsSplit(&a, "FEATURES 1");
    argsPushLen(&a, big, 100);
    argsPush(&a, "oops");
    CHECK(sectionsArgsRefused(&a));
    CHECK(strlen(g_err) == strlen("value of feature '...' must be a finite decimal number") + 64);
    CHECK(strstr(g_err, "n...' must be") != NULL);
    argsFree(&a);
}

TEST(sections_limits) {
    gateRequest req;
    testArgs a;
    char *text = malloc(65537);
    char buf[32];
    int i, pos, rc;

    if (text == NULL) testDie("out of memory");
    memset(text, 't', 65537);

    /* TEXT: empty is a present, empty section; 65536 bytes is the limit. */
    argsInit(&a);
    argsPush(&a, "TEXT");
    argsPushLen(&a, "", 0);
    pos = 0;
    CHECK_EQ_INT(sectionsArgs(&a, &pos, &req), GATE_OK);
    CHECK_EQ_INT(pos, 2);
    CHECK_EQ_INT(req.in.present, GATE_INPUT_TEXT);
    CHECK_EQ_INT(req.in.text_len, 0);
    gateRequestFree(&req);
    argsFree(&a);

    argsPush(&a, "TEXT");
    argsPushLen(&a, text, 65536);
    pos = 0;
    CHECK_EQ_INT(sectionsArgs(&a, &pos, &req), GATE_OK);
    CHECK_EQ_INT(req.in.text_len, 65536);
    gateRequestFree(&req);
    argsFree(&a);

    argsPush(&a, "TEXT");
    argsPushLen(&a, text, 65537);
    SECTIONS_ARGS_FAIL(&a, "TEXT must be at most 65536 bytes");
    argsFree(&a);
    free(text);

    /* VECTOR: any length passes here, the schema decides later. */
    argsPush(&a, "VECTOR");
    argsPushLen(&a, "", 0);
    pos = 0;
    CHECK_EQ_INT(sectionsArgs(&a, &pos, &req), GATE_OK);
    CHECK_EQ_INT(req.in.present, GATE_INPUT_VECTOR);
    CHECK_EQ_INT(req.in.vector_len, 0);
    gateRequestFree(&req);
    argsFree(&a);
    argsPush(&a, "VECTOR");
    argsPushLen(&a, "abc", 3); /* not a multiple of four bytes */
    pos = 0;
    CHECK_EQ_INT(sectionsArgs(&a, &pos, &req), GATE_OK);
    CHECK_EQ_INT(req.in.vector_len, 3);
    gateRequestFree(&req);
    argsFree(&a);

    /* FEATURES: 1024 pairs is the limit. */
    argsSplit(&a, "FEATURES 1024");
    for (i = 0; i < 1024; i++) {
        snprintf(buf, sizeof(buf), "f%d", i);
        argsPush(&a, buf);
        snprintf(buf, sizeof(buf), "%d", i - 512);
        argsPush(&a, buf);
    }
    argsPush(&a, "LABEL");
    pos = 0;
    rc = sectionsArgs(&a, &pos, &req);
    CHECK_EQ_INT(rc, GATE_OK);
    if (rc == GATE_OK) {
        CHECK_EQ_INT(pos, 2 + 2 * 1024);
        CHECK_EQ_INT(req.in.nfeatures, 1024);
        for (i = 0; i < 1024 && req.features != NULL; i++) {
            if (req.features[i].name != a.v[2 + 2 * i].ptr || req.features[i].value != i - 512)
                break;
        }
        CHECK_EQ_INT(i, 1024);
        gateRequestFree(&req);
    }
    argsFree(&a);

    argsSplit(&a, "FEATURES 1025");
    for (i = 0; i < 1025; i++) {
        snprintf(buf, sizeof(buf), "f%d", i);
        argsPush(&a, buf);
        argsPush(&a, "1");
    }
    SECTIONS_ARGS_FAIL(&a, "FEATURES count is out of range: must be at most 1024");
    argsFree(&a);
}

TEST(sections_failure_leaves_nothing) {
    gateRequest req;
    testArgs a;
    int pos;

    /* The failure comes after the features array was allocated and after
     * other sections were recorded. */
    argsInit(&a);
    argsSplit(&a, "GADD key TEXT hi FEATURES 2 a 1 b 2 VECTOR v TEXT again");
    pos = 2;
    CHECK_EQ_INT(sectionsArgs(&a, &pos, &req), GATE_ERR);
    CHECK_EQ_STR(g_err, "section TEXT given more than once");
    CHECK(requestIsZero(&req));
    CHECK_EQ_INT(pos, 2); /* not advanced */
    CHECK_EQ_INT(g_alloc_live, 0);
    /* Releasing what a failed parse left is allowed, any number of times. */
    gateRequestFree(&req);
    gateRequestFree(&req);
    gateRequestFree(NULL);
    argsFree(&a);

    /* The allocation itself failing. */
    argsSplit(&a, "TEXT hi FEATURES 2 a 1 b 2");
    g_alloc_calls = 0;
    g_alloc_fail_at = 1;
    pos = 0;
    CHECK_EQ_INT(sectionsArgs(&a, &pos, &req), GATE_ERR);
    g_alloc_fail_at = 0;
    CHECK_EQ_STR(g_err, "out of memory");
    CHECK(requestIsZero(&req));
    CHECK_EQ_INT(pos, 0);
    CHECK_EQ_INT(g_alloc_live, 0);
    /* With the allocator working again the same vector parses, and it takes
     * exactly one allocation. */
    g_alloc_calls = 0;
    CHECK_EQ_INT(sectionsArgs(&a, &pos, &req), GATE_OK);
    CHECK_EQ_INT(g_alloc_calls, 1);
    CHECK_EQ_INT(g_alloc_live, 1);
    gateRequestFree(&req);
    CHECK(requestIsZero(&req));
    CHECK_EQ_INT(g_alloc_live, 0);
    argsFree(&a);

    /* No error buffer. */
    argsSplit(&a, "TEXT");
    memset(&req, 0, sizeof(req));
    pos = 0;
    CHECK_EQ_INT(gateParseSections(a.v, a.n, &pos, &req, NULL, 0), GATE_ERR);
    CHECK(requestIsZero(&req));
    argsFree(&a);
}

/* ================================= Targets ================================ */

/* Hand built configs: the target parser is tested without going through
 * gateParseCreate(). Never released with gateConfigFree(). */
static char g_label0[] = "ignore", g_label1[] = "respond", g_label2[] = "escalate";
static char *g_labels[3] = {g_label0, g_label1, g_label2};

static gateConfig handConfig(int mode, uint32_t nclasses) {
    gateConfig cfg;

    gateConfigInit(&cfg);
    cfg.mode = mode;
    cfg.nclasses = nclasses;
    cfg.labels = nclasses > 0 ? g_labels : NULL;
    return cfg;
}

static int targetIsZero(const gateTarget *t) {
    return isAllZero(t, sizeof(*t));
}

/* 't' is zeroed first, as the contract asks of callers. */
static int targetArgs(const gateConfig *cfg, const testArgs *a, int *pos, int allow_classes,
                      int allow_correct, gateTarget *t)
{
    memset(t, 0, sizeof(*t));
    poisonError();
    return gateParseTarget(cfg, a->v, a->n, pos, allow_classes, allow_correct, t,
                           g_err, sizeof(g_err));
}

/* 1 if refused, leaving an empty target and '*pos' untouched. */
static int targetArgsRefused(const gateConfig *cfg, const testArgs *a, int allow_classes,
                             int allow_correct)
{
    gateTarget t;
    int pos = 0, rc = targetArgs(cfg, a, &pos, allow_classes, allow_correct, &t);
    int clean = targetIsZero(&t) && pos == 0 && g_alloc_live == 0;

    if (rc == GATE_OK) gateTargetFree(&t);
    if (rc != GATE_ERR) printf("    the target was accepted\n");
    else if (!clean) printf("    the target was not left empty (pos %d)\n", pos);
    return rc == GATE_ERR && clean;
}

static int targetRefused(const gateConfig *cfg, const char *line, int allow_classes,
                         int allow_correct)
{
    testArgs a;
    int refused;

    argsInit(&a);
    argsSplit(&a, line);
    refused = targetArgsRefused(cfg, &a, allow_classes, allow_correct);
    argsFree(&a);
    return refused;
}

#define TARGET_FAILS(cfg, line, allow_classes, allow_correct, msg) do { \
    CHECK(targetRefused((cfg), (line), (allow_classes), (allow_correct))); \
    CHECK_EQ_STR(g_err, (msg)); \
} while (0)

/* Parse a line that must be accepted; the caller releases the target. */
static int targetLine(const gateConfig *cfg, const char *line, int allow_classes,
                      int allow_correct, gateTarget *t, int *pos)
{
    testArgs a;
    int rc;

    argsInit(&a);
    argsSplit(&a, line);
    *pos = 0;
    rc = targetArgs(cfg, &a, pos, allow_classes, allow_correct, t);
    argsFree(&a);
    if (rc != GATE_OK) printf("    \"%s\" was refused: %s\n", line, g_err);
    return rc == GATE_OK;
}

TEST(target_label) {
    gateConfig two = handConfig(GATE_MODE_CLASSIFIER, 2);
    gateConfig three = handConfig(GATE_MODE_CLASSIFIER, 3);
    gateTarget t;
    testArgs a;
    int pos;

    CHECK(targetLine(&two, "LABEL respond", 1, 0, &t, &pos));
    CHECK_EQ_INT(pos, 2);
    CHECK_EQ_INT(t.fb.kind, GATE_FEEDBACK_LABEL);
    CHECK_EQ_INT(t.fb.label, 1);
    CHECK(t.fb.probs == NULL);
    CHECK(t.q != NULL);
    if (t.q != NULL) CHECK(t.q[0] == 0.0 && t.q[1] == 1.0);
    gateTargetFree(&t);
    CHECK(targetIsZero(&t));

    CHECK(targetLine(&two, "label ignore TEACHER router-v2", 1, 0, &t, &pos));
    CHECK_EQ_INT(pos, 2); /* TEACHER is left for the caller */
    CHECK_EQ_INT(t.fb.label, 0);
    if (t.q != NULL) CHECK(t.q[0] == 1.0 && t.q[1] == 0.0);
    gateTargetFree(&t);

    CHECK(targetLine(&three, "LABEL escalate", 1, 0, &t, &pos));
    CHECK_EQ_INT(t.fb.kind, GATE_FEEDBACK_LABEL);
    CHECK_EQ_INT(t.fb.label, 2);
    if (t.q != NULL) CHECK(t.q[0] == 0.0 && t.q[1] == 0.0 && t.q[2] == 1.0);
    gateTargetFree(&t);
    CHECK(targetLine(&three, "LABEL respond", 1, 0, &t, &pos));
    CHECK_EQ_INT(t.fb.label, 1);
    if (t.q != NULL) CHECK(t.q[0] == 0.0 && t.q[1] == 1.0 && t.q[2] == 0.0);
    gateTargetFree(&t);

    /* The label must be one of the K configured ones: with K = 2 the third
     * entry of the label array is not a class. */
    TARGET_FAILS(&two, "LABEL escalate", 1, 0, "unknown class label 'escalate'");
    TARGET_FAILS(&three, "LABEL nope", 1, 0, "unknown class label 'nope'");
    TARGET_FAILS(&three, "LABEL Respond", 1, 0, "unknown class label 'Respond'");
    TARGET_FAILS(&three, "LABEL respon", 1, 0, "unknown class label 'respon'");
    TARGET_FAILS(&three, "LABEL responder", 1, 0, "unknown class label 'responder'");
    TARGET_FAILS(&three, "LABEL 1", 1, 0, "unknown class label '1'");
    TARGET_FAILS(&three, "LABEL", 1, 0, "LABEL requires a class label");

    argsInit(&a);
    argsPush(&a, "LABEL");
    argsPushLen(&a, "", 0);
    CHECK(targetArgsRefused(&three, &a, 1, 0));
    CHECK_EQ_STR(g_err, "unknown class label ''");
    argsFree(&a);
    argsPush(&a, "LABEL");
    argsPushLen(&a, LIT("respond\0"));
    CHECK(targetArgsRefused(&three, &a, 1, 0));
    CHECK_EQ_STR(g_err, "unknown class label 'respond?'");
    argsFree(&a);
    argsPush(&a, "LABEL");
    argsPushLen(&a, LIT("re\r\nspond"));
    CHECK(targetArgsRefused(&three, &a, 1, 0));
    CHECK_EQ_STR(g_err, "unknown class label 're??spond'");
    argsFree(&a);
}

TEST(target_probs) {
    gateConfig two = handConfig(GATE_MODE_CLASSIFIER, 2);
    gateConfig three = handConfig(GATE_MODE_CLASSIFIER, 3);
    gateTarget t;
    int pos;

    /* Sums that are exactly 1 in binary: the values come back untouched. */
    CHECK(targetLine(&two, "PROBS 0.25 0.75", 1, 0, &t, &pos));
    CHECK_EQ_INT(pos, 3);
    CHECK_EQ_INT(t.fb.kind, GATE_FEEDBACK_PROBS);
    CHECK_EQ_INT(t.fb.label, 1);
    CHECK(t.q != NULL && t.fb.probs == t.q);
    if (t.q != NULL) CHECK(t.q[0] == 0.25 && t.q[1] == 0.75);
    gateTargetFree(&t);
    CHECK(targetIsZero(&t));

    CHECK(targetLine(&three, "probs 0.2 0.3 0.5 TEACHER v2", 1, 0, &t, &pos));
    CHECK_EQ_INT(pos, 4); /* exactly K values are consumed */
    CHECK_EQ_INT(t.fb.kind, GATE_FEEDBACK_PROBS);
    CHECK_EQ_INT(t.fb.label, 2);
    CHECK(t.fb.probs == t.q);
    if (t.q != NULL) CHECK(t.q[0] == 0.2 && t.q[1] == 0.3 && t.q[2] == 0.5);
    gateTargetFree(&t);

    /* A hard label written as probabilities. */
    CHECK(targetLine(&three, "PROBS 0 1 0", 1, 0, &t, &pos));
    CHECK_EQ_INT(t.fb.label, 1);
    if (t.q != NULL) CHECK(t.q[0] == 0.0 && t.q[1] == 1.0 && t.q[2] == 0.0);
    gateTargetFree(&t);
    CHECK(targetLine(&two, "PROBS 1e0 0.0", 1, 0, &t, &pos));
    CHECK_EQ_INT(t.fb.label, 0);
    if (t.q != NULL) CHECK(t.q[0] == 1.0 && t.q[1] == 0.0);
    gateTargetFree(&t);

    /* Within the tolerance: renormalized to sum to 1. The expected values
     * are p / sum computed in exact rational arithmetic. */
    CHECK(targetLine(&two, "PROBS 0.4995 0.5", 1, 0, &t, &pos));
    CHECK_EQ_INT(t.fb.label, 1);
    if (t.q != NULL) {
        CHECK_NEAR(t.q[0], 0.49974987493746875, 1e-15);
        CHECK_NEAR(t.q[1], 0.5002501250625313, 1e-15);
        CHECK_NEAR(t.q[0] + t.q[1], 1.0, 1e-15);
    }
    gateTargetFree(&t);
    CHECK(targetLine(&two, "PROBS 0.5005 0.5", 1, 0, &t, &pos));
    CHECK_EQ_INT(t.fb.label, 0);
    if (t.q != NULL) {
        CHECK_NEAR(t.q[0], 0.5002498750624688, 1e-15);
        CHECK_NEAR(t.q[1], 0.49975012493753124, 1e-15);
        CHECK_NEAR(t.q[0] + t.q[1], 1.0, 1e-15);
    }
    gateTargetFree(&t);
    CHECK(targetLine(&three, "PROBS 0.2 0.3 0.5005", 1, 0, &t, &pos));
    CHECK_EQ_INT(t.fb.label, 2);
    if (t.q != NULL) {
        CHECK_NEAR(t.q[0], 0.19990004997501248, 1e-15);
        CHECK_NEAR(t.q[1], 0.29985007496251875, 1e-15);
        CHECK_NEAR(t.q[2], 0.5002498750624688, 1e-15);
        CHECK_NEAR(t.q[0] + t.q[1] + t.q[2], 1.0, 1e-15);
    }
    gateTargetFree(&t);
    CHECK(targetLine(&three, "PROBS 0.25 0.25 0.4995", 1, 0, &t, &pos));
    CHECK_EQ_INT(t.fb.label, 2);
    if (t.q != NULL) {
        CHECK_NEAR(t.q[0], 0.25012506253126565, 1e-15);
        CHECK_NEAR(t.q[1], 0.25012506253126565, 1e-15);
        CHECK_NEAR(t.q[2], 0.49974987493746875, 1e-15);
    }
    gateTargetFree(&t);

    /* More values than classes: the extra one is the caller's problem. */
    CHECK(targetLine(&two, "PROBS 0.5 0.5 0.0", 1, 0, &t, &pos));
    CHECK_EQ_INT(pos, 3);
    gateTargetFree(&t);
}

TEST(target_probs_argmax_and_ties) {
    static const struct {
        uint32_t k;
        const char *line;
        int label;
    } cases[] = {
        {2, "PROBS 0.5 0.5", 0},       /* a tie goes to the lowest index */
        {2, "PROBS 0.6 0.4", 0},
        {2, "PROBS 0.4 0.6", 1},
        {2, "PROBS 0.49999 0.50001", 1},
        {3, "PROBS 0.4 0.4 0.2", 0},
        {3, "PROBS 0.2 0.4 0.4", 1},   /* the tie is not at index 0 */
        {3, "PROBS 0.4 0.2 0.4", 0},
        {3, "PROBS 0.25 0.5 0.25", 1},
        {3, "PROBS 0.1 0.2 0.7", 2},
        {3, "PROBS 0.7 0.2 0.1", 0},
        {3, "PROBS 0.3333 0.3333 0.3334", 2},
        {3, "PROBS 0.3334 0.3333 0.3333", 0},
        {3, "PROBS 0.3333 0.3334 0.3333", 1},
        {3, "PROBS 0.333 0.333 0.333", 0}, /* three way tie, renormalized */
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        gateConfig cfg = handConfig(GATE_MODE_CLASSIFIER, cases[i].k);
        gateTarget t;
        int pos;

        CHECK(targetLine(&cfg, cases[i].line, 1, 0, &t, &pos));
        CHECK_EQ_INT(t.fb.label, cases[i].label);
        gateTargetFree(&t);
    }
}

TEST(target_probs_tolerance) {
    gateConfig two = handConfig(GATE_MODE_CLASSIFIER, 2);
    gateConfig three = handConfig(GATE_MODE_CLASSIFIER, 3);
    static const char *const accepted[] = {
        "PROBS 0.4995 0.5", "PROBS 0.5005 0.5", /* 5e-4 off */
        "PROBS 0.4991 0.5", "PROBS 0.5009 0.5", /* 9e-4 off */
        /* Exactly 1e-3 off in decimal. In binary the first sum is a hair
         * outside and the second a hair inside; both are "within 0.001". */
        "PROBS 0.499 0.5", "PROBS 0.501 0.5",
    };
    static const char *const rejected[] = {
        "PROBS 0.4989 0.5", "PROBS 0.5011 0.5",       /* 1.1e-3 off */
        "PROBS 0.49899 0.5", "PROBS 0.50101 0.5",     /* 1.01e-3 off */
        "PROBS 0.4989999 0.5", "PROBS 0.5010001 0.5", /* 1.0000001e-3 off */
        "PROBS 0 0", "PROBS 0.5 0.4", "PROBS 1 1", "PROBS 0.9 0.9", "PROBS 0.1 0.1",
        "PROBS 100 0", "PROBS 1e308 1e308", /* the sum overflows */
    };
    gateTarget t;
    size_t i;
    int pos;

    for (i = 0; i < sizeof(accepted) / sizeof(accepted[0]); i++) {
        CHECK(targetLine(&two, accepted[i], 1, 0, &t, &pos));
        if (t.q != NULL) CHECK_NEAR(t.q[0] + t.q[1], 1.0, 1e-15);
        gateTargetFree(&t);
    }
    for (i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++)
        TARGET_FAILS(&two, rejected[i], 1, 0, "PROBS must sum to 1");

    /* Three classes rounded to three decimals sit exactly on the boundary. */
    CHECK(targetLine(&three, "PROBS 0.333 0.333 0.333", 1, 0, &t, &pos));
    if (t.q != NULL) {
        CHECK_NEAR(t.q[0], 1.0 / 3.0, 1e-15);
        CHECK(t.q[0] == t.q[1] && t.q[1] == t.q[2]);
    }
    gateTargetFree(&t);
    CHECK(targetLine(&three, "PROBS 0.3337 0.3337 0.3336", 1, 0, &t, &pos));
    gateTargetFree(&t);
    TARGET_FAILS(&three, "PROBS 0.333 0.333 0.3329", 1, 0, "PROBS must sum to 1");
    TARGET_FAILS(&three, "PROBS 0.3337 0.3337 0.3337", 1, 0, "PROBS must sum to 1");
}

TEST(target_probs_invalid_values_and_counts) {
    gateConfig two = handConfig(GATE_MODE_CLASSIFIER, 2);
    gateConfig three = handConfig(GATE_MODE_CLASSIFIER, 3);
    testArgs a;

    /* Too few arguments left. */
    TARGET_FAILS(&two, "PROBS", 1, 0, "PROBS requires 2 values");
    TARGET_FAILS(&two, "PROBS 1", 1, 0, "PROBS requires 2 values");
    TARGET_FAILS(&three, "PROBS", 1, 0, "PROBS requires 3 values");
    TARGET_FAILS(&three, "PROBS 0.5 0.5", 1, 0, "PROBS requires 3 values");
    /* One short, and the next token is taken for a value. */
    TARGET_FAILS(&three, "PROBS 0.5 0.5 TEACHER v1", 1, 0,
                 "PROBS requires 3 values: 'TEACHER' is not a finite non-negative number");

    /* Negative, even when the sum is 1. */
    TARGET_FAILS(&two, "PROBS -0.1 1.1", 1, 0,
                 "PROBS requires 2 values: '-0.1' is not a finite non-negative number");
    TARGET_FAILS(&two, "PROBS 1.5 -0.5", 1, 0,
                 "PROBS requires 2 values: '-0.5' is not a finite non-negative number");
    TARGET_FAILS(&three, "PROBS 0.5 0.5 -1e-9", 1, 0,
                 "PROBS requires 3 values: '-1e-9' is not a finite non-negative number");
    /* Not finite, not a number. */
    TARGET_FAILS(&two, "PROBS nan 1", 1, 0,
                 "PROBS requires 2 values: 'nan' is not a finite non-negative number");
    TARGET_FAILS(&two, "PROBS 1 NaN", 1, 0,
                 "PROBS requires 2 values: 'NaN' is not a finite non-negative number");
    TARGET_FAILS(&two, "PROBS 0.5 inf", 1, 0,
                 "PROBS requires 2 values: 'inf' is not a finite non-negative number");
    TARGET_FAILS(&two, "PROBS 1e999 0", 1, 0,
                 "PROBS requires 2 values: '1e999' is not a finite non-negative number");
    TARGET_FAILS(&two, "PROBS 0.5 abc", 1, 0,
                 "PROBS requires 2 values: 'abc' is not a finite non-negative number");
    TARGET_FAILS(&two, "PROBS 0.5 0x1p-1", 1, 0,
                 "PROBS requires 2 values: '0x1p-1' is not a finite non-negative number");
    TARGET_FAILS(&two, "PROBS 50% 50%", 1, 0,
                 "PROBS requires 2 values: '50%' is not a finite non-negative number");

    argsInit(&a);
    argsSplit(&a, "PROBS 1");
    argsPushLen(&a, "", 0);
    CHECK(targetArgsRefused(&two, &a, 1, 0));
    CHECK_EQ_STR(g_err, "PROBS requires 2 values: '' is not a finite non-negative number");
    argsFree(&a);
    argsSplit(&a, "PROBS 0.5");
    argsPushLen(&a, LIT("0.5\0"));
    CHECK(targetArgsRefused(&two, &a, 1, 0));
    CHECK_EQ_STR(g_err, "PROBS requires 2 values: '0.5?' is not a finite non-negative number");
    argsFree(&a);
}

TEST(target_correct) {
    static const char *const bad[] = {"CORRECT", "CORRECT 2", "CORRECT 01", "CORRECT 10",
                                      "CORRECT 00", "CORRECT 11", "CORRECT +1", "CORRECT -1",
                                      "CORRECT 1.0", "CORRECT 0.0", "CORRECT true",
                                      "CORRECT yes", "CORRECT y", "CORRECT T"};
    gateConfig score = handConfig(GATE_MODE_SCORE, 0);
    gateTarget t;
    testArgs a;
    size_t i;
    int pos;

    CHECK(targetLine(&score, "CORRECT 1", 0, 1, &t, &pos));
    CHECK_EQ_INT(pos, 2);
    CHECK_EQ_INT(t.fb.kind, GATE_FEEDBACK_CORRECT);
    CHECK_EQ_INT(t.fb.correct, 1);
    CHECK_EQ_INT(t.fb.label, -1);
    CHECK(t.q == NULL && t.fb.probs == NULL);
    gateTargetFree(&t);

    CHECK(targetLine(&score, "correct 0 TEACHER v2", 0, 1, &t, &pos));
    CHECK_EQ_INT(pos, 2);
    CHECK_EQ_INT(t.fb.kind, GATE_FEEDBACK_CORRECT);
    CHECK_EQ_INT(t.fb.correct, 0);
    CHECK(t.q == NULL && t.fb.probs == NULL);
    gateTargetFree(&t);

    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        TARGET_FAILS(&score, bad[i], 0, 1, "CORRECT must be 0 or 1");
    argsInit(&a);
    argsPush(&a, "CORRECT");
    argsPushLen(&a, "", 0);
    CHECK(targetArgsRefused(&score, &a, 0, 1));
    CHECK_EQ_STR(g_err, "CORRECT must be 0 or 1");
    argsFree(&a);
    argsPush(&a, "CORRECT");
    argsPushLen(&a, LIT("1\0"));
    CHECK(targetArgsRefused(&score, &a, 0, 1));
    CHECK_EQ_STR(g_err, "CORRECT must be 0 or 1");
    argsFree(&a);
    argsPush(&a, "CORRECT");
    argsPush(&a, "1 ");
    CHECK(targetArgsRefused(&score, &a, 0, 1));
    CHECK_EQ_STR(g_err, "CORRECT must be 0 or 1");
    argsFree(&a);
}

TEST(target_switches) {
    gateConfig classifier = handConfig(GATE_MODE_CLASSIFIER, 3);
    gateConfig score = handConfig(GATE_MODE_SCORE, 0);
    gateConfig score_classes = handConfig(GATE_MODE_SCORE, 2);
    gateTarget t;
    testArgs none;
    int pos;

    argsInit(&none);

    /* GADD, and GLEARN on a CLASSIFIER gate: classes yes, CORRECT no. */
    TARGET_FAILS(&classifier, "CORRECT 1", 1, 0,
                 "CORRECT is not accepted here: expected LABEL or PROBS");
    TARGET_FAILS(&classifier, "FOO", 1, 0, "expected LABEL or PROBS");
    TARGET_FAILS(&classifier, "TEACHER v2", 1, 0, "expected LABEL or PROBS");
    CHECK(targetArgsRefused(&classifier, &none, 1, 0));
    CHECK_EQ_STR(g_err, "expected LABEL or PROBS");

    /* GLEARN on a SCORE gate: CORRECT only, with or without CLASSES. */
    TARGET_FAILS(&score, "LABEL x", 0, 1, "LABEL is not accepted here: expected CORRECT");
    TARGET_FAILS(&score, "PROBS 1", 0, 1, "PROBS is not accepted here: expected CORRECT");
    TARGET_FAILS(&score, "FOO", 0, 1, "expected CORRECT");
    CHECK(targetArgsRefused(&score, &none, 0, 1));
    CHECK_EQ_STR(g_err, "expected CORRECT");
    TARGET_FAILS(&score_classes, "LABEL ignore", 0, 1,
                 "LABEL is not accepted here: expected CORRECT");
    TARGET_FAILS(&score_classes, "PROBS 0.5 0.5", 0, 1,
                 "PROBS is not accepted here: expected CORRECT");
    CHECK(targetLine(&score_classes, "CORRECT 1", 0, 1, &t, &pos));
    CHECK_EQ_INT(t.fb.kind, GATE_FEEDBACK_CORRECT);
    gateTargetFree(&t);

    /* The LABEL of "GPREDICT key SCORE s LABEL l" on a SCORE gate with
     * classes: a label, but never a distribution. */
    CHECK(targetLine(&score_classes, "LABEL respond", 1, 0, &t, &pos));
    CHECK_EQ_INT(t.fb.kind, GATE_FEEDBACK_LABEL);
    CHECK_EQ_INT(t.fb.label, 1);
    gateTargetFree(&t);
    TARGET_FAILS(&score_classes, "PROBS 0.5 0.5", 1, 0, "PROBS is only accepted by CLASSIFIER gates");
    TARGET_FAILS(&score_classes, "CORRECT 1", 1, 0, "CORRECT is not accepted here: expected LABEL");
    TARGET_FAILS(&score_classes, "FOO", 1, 0, "expected LABEL");
    TARGET_FAILS(&score_classes, "FOO", 1, 1, "expected LABEL or CORRECT");

    /* A gate without classes has no labels to name. */
    TARGET_FAILS(&score, "LABEL x", 1, 1, "LABEL requires a gate with CLASSES");
    TARGET_FAILS(&score, "LABEL", 1, 1, "LABEL requires a gate with CLASSES");
    TARGET_FAILS(&score, "PROBS 1", 1, 1, "PROBS is only accepted by CLASSIFIER gates");
    TARGET_FAILS(&score, "FOO", 1, 1, "expected CORRECT");
    TARGET_FAILS(&score, "LABEL x", 1, 0, "LABEL requires a gate with CLASSES");

    /* Everything allowed. */
    TARGET_FAILS(&classifier, "FOO", 1, 1, "expected LABEL, PROBS or CORRECT");
    CHECK(targetArgsRefused(&classifier, &none, 1, 1));
    CHECK_EQ_STR(g_err, "expected LABEL, PROBS or CORRECT");
    CHECK(targetLine(&classifier, "CORRECT 1", 1, 1, &t, &pos));
    CHECK_EQ_INT(t.fb.kind, GATE_FEEDBACK_CORRECT);
    gateTargetFree(&t);
    CHECK(targetLine(&classifier, "LABEL ignore", 1, 1, &t, &pos));
    CHECK_EQ_INT(t.fb.kind, GATE_FEEDBACK_LABEL);
    gateTargetFree(&t);

    /* Nothing allowed. */
    TARGET_FAILS(&classifier, "LABEL ignore", 0, 0, "LABEL is not accepted here");
    TARGET_FAILS(&classifier, "PROBS 1 0 0", 0, 0, "PROBS is not accepted here");
    TARGET_FAILS(&classifier, "CORRECT 1", 0, 0, "CORRECT is not accepted here");
    TARGET_FAILS(&classifier, "FOO", 0, 0, "expected LABEL, PROBS or CORRECT");
}

TEST(target_position_and_failures) {
    gateConfig classifier = handConfig(GATE_MODE_CLASSIFIER, 3);
    gateTarget t;
    testArgs a;
    int pos;

    /* The target is read where '*pos' says, not at the start. */
    argsInit(&a);
    argsSplit(&a, "LABEL ignore LABEL respond PROBS 0 0 1 TEACHER v");
    pos = 2;
    CHECK_EQ_INT(targetArgs(&classifier, &a, &pos, 1, 0, &t), GATE_OK);
    CHECK_EQ_INT(pos, 4);
    CHECK_EQ_INT(t.fb.label, 1);
    gateTargetFree(&t);
    CHECK_EQ_INT(targetArgs(&classifier, &a, &pos, 1, 0, &t), GATE_OK);
    CHECK_EQ_INT(pos, 8);
    CHECK_EQ_INT(t.fb.kind, GATE_FEEDBACK_PROBS);
    CHECK_EQ_INT(t.fb.label, 2);
    gateTargetFree(&t);
    /* At TEACHER, and at the end: no target, position unchanged. */
    CHECK_EQ_INT(targetArgs(&classifier, &a, &pos, 1, 0, &t), GATE_ERR);
    CHECK_EQ_INT(pos, 8);
    CHECK(targetIsZero(&t));
    pos = a.n;
    CHECK_EQ_INT(targetArgs(&classifier, &a, &pos, 1, 0, &t), GATE_ERR);
    CHECK_EQ_STR(g_err, "expected LABEL or PROBS");
    CHECK_EQ_INT(pos, a.n);

    /* A failure after the distribution was allocated. */
    pos = 4;
    argsFree(&a);
    argsSplit(&a, "GLEARN key OBSERVATION id PROBS 0.5 0.5 0.5");
    CHECK_EQ_INT(targetArgs(&classifier, &a, &pos, 1, 0, &t), GATE_ERR);
    CHECK_EQ_STR(g_err, "PROBS must sum to 1");
    CHECK(targetIsZero(&t));
    CHECK_EQ_INT(pos, 4);
    CHECK_EQ_INT(g_alloc_live, 0);
    gateTargetFree(&t);
    gateTargetFree(&t);
    gateTargetFree(NULL);
    argsFree(&a);

    /* The allocation failing, for both forms that allocate. */
    argsSplit(&a, "PROBS 0.2 0.3 0.5");
    g_alloc_calls = 0;
    g_alloc_fail_at = 1;
    pos = 0;
    CHECK_EQ_INT(targetArgs(&classifier, &a, &pos, 1, 0, &t), GATE_ERR);
    g_alloc_fail_at = 0;
    CHECK_EQ_STR(g_err, "out of memory");
    CHECK(targetIsZero(&t) && pos == 0);
    CHECK_EQ_INT(g_alloc_live, 0);
    g_alloc_calls = 0;
    CHECK_EQ_INT(targetArgs(&classifier, &a, &pos, 1, 0, &t), GATE_OK);
    CHECK_EQ_INT(g_alloc_calls, 1);
    gateTargetFree(&t);
    argsFree(&a);

    argsSplit(&a, "LABEL respond");
    g_alloc_calls = 0;
    g_alloc_fail_at = 1;
    pos = 0;
    CHECK_EQ_INT(targetArgs(&classifier, &a, &pos, 1, 0, &t), GATE_ERR);
    g_alloc_fail_at = 0;
    CHECK_EQ_STR(g_err, "out of memory");
    CHECK(targetIsZero(&t) && pos == 0);
    CHECK_EQ_INT(g_alloc_live, 0);

    /* No error buffer. */
    memset(&t, 0, sizeof(t));
    pos = 1;
    CHECK_EQ_INT(gateParseTarget(&classifier, a.v, a.n, &pos, 1, 0, &t, NULL, 0), GATE_ERR);
    CHECK(targetIsZero(&t) && pos == 1);
    argsFree(&a);
}

/* ============================ Reset scope, flows ========================== */

TEST(reset_scope) {
    static const struct {
        const char *s;
        gateResetScope scope;
    } good[] = {
        {"CANDIDATE", GATE_RESET_CANDIDATE}, {"CALIBRATION", GATE_RESET_CALIBRATION},
        {"MODEL", GATE_RESET_MODEL},         {"ALL", GATE_RESET_ALL},
        {"candidate", GATE_RESET_CANDIDATE}, {"Calibration", GATE_RESET_CALIBRATION},
        {"mOdEl", GATE_RESET_MODEL},         {"all", GATE_RESET_ALL},
    };
    static const char *const bad[] = {"", "CANDIDATES", "CANDIDAT", "CALIBRATE", "MODELS", "AL",
                                      "ALLL", "ALL ", " ALL", "EVERYTHING", "0", "TRAINER"};
    size_t i;

    for (i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        gateArg a = {exactCopy(good[i].s, strlen(good[i].s)), strlen(good[i].s)};
        gateResetScope scope = (gateResetScope)99;

        CHECK(gateParseResetScope(&a, &scope) == 1);
        CHECK_EQ_INT(scope, good[i].scope);
        free((void *)a.ptr);
    }
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        gateArg a = {exactCopy(bad[i], strlen(bad[i])), strlen(bad[i])};
        gateResetScope scope = (gateResetScope)99;

        CHECK(gateParseResetScope(&a, &scope) == 0);
        CHECK_EQ_INT(scope, 99); /* left alone */
        free((void *)a.ptr);
    }
    {
        gateArg a = {exactCopy(LIT("ALL\0")), 4};
        gateResetScope scope = (gateResetScope)99;

        CHECK(gateParseResetScope(&a, &scope) == 0);
        free((void *)a.ptr);
    }
}

/* The README quick start, argument by argument, the way gate.c will drive
 * the three parsers. */
TEST(readme_classifier_flow) {
    gateConfig cfg;
    gateRequest req;
    gateTarget t;
    testArgs a;
    uint64_t epoch = 0, seq = 0;
    int pos;

    CREATE_OK_OR_RETURN("MODE CLASSIFIER CLASSES 2 ignore respond INPUTS 2 TEXT FEATURES "
                        "ALPHA 0.02 TEACHER router-v1 SCHEMAVERSION chat-v1", &cfg);

    /* GADD agent:respond TEXT "Please explain this" FEATURES 2 mentioned 1
     *      previous_speaker_agent 0 LABEL respond */
    argsInit(&a);
    argsSplit(&a, "GADD agent:respond TEXT");
    argsPush(&a, "Please explain this");
    argsSplit(&a, "FEATURES 2 mentioned 1 previous_speaker_agent 0 LABEL respond");
    pos = 2;
    CHECK_EQ_INT(sectionsArgs(&a, &pos, &req), GATE_OK);
    CHECK_EQ_INT(pos, 10); /* at LABEL */
    CHECK_EQ_INT(req.in.present, cfg.schema.inputs);
    CHECK(req.in.text_len == 19 && memcmp(req.in.text, "Please explain this", 19) == 0);
    CHECK_EQ_INT(req.in.nfeatures, 2);
    CHECK_EQ_INT(targetArgs(&cfg, &a, &pos, 1, 0, &t), GATE_OK);
    CHECK_EQ_INT(pos, a.n); /* the whole command was consumed */
    CHECK_EQ_INT(t.fb.kind, GATE_FEEDBACK_LABEL);
    CHECK_EQ_INT(t.fb.label, 1);
    if (t.q != NULL) CHECK(t.q[0] == 0.0 && t.q[1] == 1.0);
    gateTargetFree(&t);
    gateRequestFree(&req);
    argsFree(&a);

    /* GPREDICT agent:respond TEXT "thank you so much" FEATURES 2 mentioned 0
     *          previous_speaker_agent 0 */
    argsSplit(&a, "GPREDICT agent:respond TEXT");
    argsPush(&a, "thank you so much");
    argsSplit(&a, "FEATURES 2 mentioned 0 previous_speaker_agent 0");
    pos = 2;
    CHECK_EQ_INT(sectionsArgs(&a, &pos, &req), GATE_OK);
    CHECK_EQ_INT(pos, a.n);
    gateRequestFree(&req);
    argsFree(&a);

    /* GLEARN agent:respond OBSERVATION 0b7e41c2a9d3f586-1 LABEL ignore */
    argsSplit(&a, "GLEARN agent:respond OBSERVATION 0b7e41c2a9d3f586-1 LABEL ignore");
    CHECK(gateArgIs(&a.v[2], "OBSERVATION"));
    CHECK(gateParseObservationId(a.v[3].ptr, a.v[3].len, &epoch, &seq));
    CHECK(epoch == 0x0b7e41c2a9d3f586ull && seq == 1);
    pos = 4;
    CHECK_EQ_INT(targetArgs(&cfg, &a, &pos, 1, 0, &t), GATE_OK);
    CHECK_EQ_INT(pos, a.n);
    CHECK_EQ_INT(t.fb.label, 0);
    gateTargetFree(&t);
    argsFree(&a);

    gateConfigFree(&cfg);
}

TEST(readme_score_flow) {
    gateConfig cfg;
    gateTarget t;
    testArgs a;
    uint64_t epoch = 0, seq = 0;
    double score = 0;
    int pos;

    CREATE_OK_OR_RETURN("MODE SCORE ALPHA 0.05 TEACHER equivalence-v1 SCHEMAVERSION cosine-v1",
                        &cfg);

    /* GPREDICT cache:gate SCORE 0.93 */
    argsInit(&a);
    argsSplit(&a, "GPREDICT cache:gate SCORE 0.93");
    CHECK(gateArgIs(&a.v[2], "SCORE"));
    CHECK(gateParseDouble(a.v[3].ptr, a.v[3].len, &score) && score == 0.93);
    argsFree(&a);

    /* GLEARN cache:gate OBSERVATION 5d0c3f9a7b21e684-1 CORRECT 1 */
    argsSplit(&a, "GLEARN cache:gate OBSERVATION 5d0c3f9a7b21e684-1 CORRECT 1");
    CHECK(gateParseObservationId(a.v[3].ptr, a.v[3].len, &epoch, &seq));
    CHECK(epoch == 0x5d0c3f9a7b21e684ull && seq == 1);
    pos = 4;
    CHECK_EQ_INT(targetArgs(&cfg, &a, &pos, 0, 1, &t), GATE_OK);
    CHECK_EQ_INT(pos, a.n);
    CHECK_EQ_INT(t.fb.kind, GATE_FEEDBACK_CORRECT);
    CHECK_EQ_INT(t.fb.correct, 1);
    gateTargetFree(&t);
    argsFree(&a);

    gateConfigFree(&cfg);
}

/* Run a test and require that it released every block it got from the
 * library. */
#define RUN_NO_LEAK(name) do { \
    RUN(name); \
    CHECK_EQ_INT(g_alloc_live, 0); \
    g_alloc_live = 0; \
} while (0)

int main(void) {
    gateSetAllocator(testMalloc, testCalloc, testRealloc, testFree);

    RUN_NO_LEAK(double_accepts_decimal_syntax);
    RUN_NO_LEAK(double_rejects_everything_else);
    RUN_NO_LEAK(double_reads_exactly_len_bytes);
    RUN_NO_LEAK(u64_accepts_digits_only);
    RUN_NO_LEAK(u64_rejects_everything_else);
    RUN_NO_LEAK(arg_is_folds_ascii_letters_only);
    RUN_NO_LEAK(observation_id_format);
    RUN_NO_LEAK(observation_id_round_trip);
    RUN_NO_LEAK(observation_id_parse_is_strict);
    RUN_NO_LEAK(hex64_format_and_parse);
    RUN_NO_LEAK(double_bits_known_patterns);
    RUN_NO_LEAK(double_bits_round_trip_and_strictness);

    RUN_NO_LEAK(create_spec_example_classifier);
    RUN_NO_LEAK(create_spec_example_score);
    RUN_NO_LEAK(create_readme_quick_start);
    RUN_NO_LEAK(create_minimal_commands_get_every_default);
    RUN_NO_LEAK(create_every_option_is_stored);
    RUN_NO_LEAK(create_option_order_does_not_matter);
    RUN_NO_LEAK(create_cold_audit_follows_audit);
    RUN_NO_LEAK(create_is_case_insensitive_for_tokens_only);
    RUN_NO_LEAK(create_requires_mode_and_alpha);
    RUN_NO_LEAK(create_rejects_repeated_options);
    RUN_NO_LEAK(create_rejects_truncated_commands);
    RUN_NO_LEAK(create_rejects_unknown_tokens);
    RUN_NO_LEAK(create_labels_may_look_like_options);
    RUN_NO_LEAK(create_number_syntax_errors);
    RUN_NO_LEAK(create_double_ranges);
    RUN_NO_LEAK(create_integer_ranges);
    RUN_NO_LEAK(create_class_count_limits);
    RUN_NO_LEAK(create_class_policy);
    RUN_NO_LEAK(create_score_mode_rejects_classifier_options);
    RUN_NO_LEAK(create_classifier_dimensions);
    RUN_NO_LEAK(create_maxmemory_must_hold_the_model);
    RUN_NO_LEAK(create_names_follow_the_utf8_policy);
    RUN_NO_LEAK(create_messages_are_one_printable_line);
    RUN_NO_LEAK(create_error_buffer_edge_cases);
    RUN_NO_LEAK(create_failure_releases_everything);
    RUN_NO_LEAK(create_survives_every_allocation_failure);

    RUN_NO_LEAK(sections_in_every_order_and_subset);
    RUN_NO_LEAK(sections_stop_at_the_first_foreign_token);
    RUN_NO_LEAK(sections_reject_repeats_and_truncation);
    RUN_NO_LEAK(sections_features_counts);
    RUN_NO_LEAK(sections_feature_values_and_names);
    RUN_NO_LEAK(sections_limits);
    RUN_NO_LEAK(sections_failure_leaves_nothing);

    RUN_NO_LEAK(target_label);
    RUN_NO_LEAK(target_probs);
    RUN_NO_LEAK(target_probs_argmax_and_ties);
    RUN_NO_LEAK(target_probs_tolerance);
    RUN_NO_LEAK(target_probs_invalid_values_and_counts);
    RUN_NO_LEAK(target_correct);
    RUN_NO_LEAK(target_switches);
    RUN_NO_LEAK(target_position_and_failures);

    RUN_NO_LEAK(reset_scope);
    RUN_NO_LEAK(readme_classifier_flow);
    RUN_NO_LEAK(readme_score_flow);

    gateSetAllocator(malloc, calloc, realloc, free);
    return TEST_REPORT();
}

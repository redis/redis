/*
 * Gate data type: command arguments. See gate_args.h for the grammar.
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * An argument is whatever bytes a client sent: it is not NUL-terminated and
 * may hold anything, NULs included. Nothing here reads past its length or
 * takes it for a C string.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gate_args.h"

/* Refusing "1e999" or the bits of a NaN is an isfinite() test, which
 * -ffast-math allows the compiler to assume away. */
#if defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__
#error "gate_args.c needs IEEE NaN and infinity semantics: do not build it with -ffast-math"
#endif

#define GATE_ARGS_FAIL(...) do { \
    if (err && errlen) snprintf(err, errlen, __VA_ARGS__); \
    return GATE_ERR; \
} while (0)

/* What an error message shows of an argument. The message becomes one line
 * of a reply while the argument is client bytes, so only a valid name is
 * quoted (UTF-8 without control characters, at most 64 bytes: every label
 * qualifies). Anything else is shown as "?". */
static const gateArg *gateArgShown(const gateArg *a) {
    static const gateArg hidden = {"?", 1};

    return gateValidName(a->ptr, a->len, GATE_MAX_LABEL_BYTES) ? a : &hidden;
}

/* The two printf arguments of a "%.*s". */
#define GATE_SHOWN(a) (int)gateArgShown(a)->len, gateArgShown(a)->ptr

/* --------------------------- Scalar helpers ------------------------------ */

int gateParseDouble(const char *s, size_t len, double *out) {
    /* Room for the exact decimal expansion of any double (the longest has
     * about 1,080 characters): a number written out in full is a number. */
    char buf[1100], *end;
    double v;
    size_t i;

    if (len == 0 || len >= sizeof(buf)) return 0;
    /* strtod() takes more than decimal numbers: leading space, hex floats,
     * "inf", "nan". Each of those needs a byte outside this alphabet. So
     * does a NUL, which would end the copy early and let "1\0junk" pass
     * as 1. Any other malformed input is caught by requiring strtod() to
     * consume everything. (In a locale with another decimal point it
     * stops at the '.': the number is refused, never misread.) */
    for (i = 0; i < len; i++) {
        char c = s[i];

        if (!((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.' ||
              c == 'e' || c == 'E'))
            return 0;
    }
    memcpy(buf, s, len);
    buf[len] = '\0';
    v = strtod(buf, &end);
    if (end != buf + len || !isfinite(v)) return 0; /* overflow is +-inf */
    *out = v;
    return 1;
}

int gateParseU64(const char *s, size_t len, uint64_t *out) {
    uint64_t v = 0;
    size_t i;

    if (len == 0 || len > 20) return 0; /* UINT64_MAX has 20 digits */
    for (i = 0; i < len; i++) {
        unsigned d;

        if (s[i] < '0' || s[i] > '9') return 0;
        d = (unsigned)(s[i] - '0');
        if (v > (UINT64_MAX - d) / 10) return 0; /* v * 10 + d would wrap */
        v = v * 10 + d;
    }
    *out = v;
    return 1;
}

int gateArgIs(const gateArg *a, const char *word) {
    size_t i;

    for (i = 0; i < a->len; i++) {
        char c = a->ptr[i];

        /* Folded by hand: toupper() depends on the locale. */
        if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
        /* The end of the word is tested first, so an argument that goes on
         * after it, even with a NUL, does not match. */
        if (word[i] == '\0' || c != word[i]) return 0;
    }
    return word[i] == '\0';
}

void gateFormatHex64(char *buf, uint64_t v) {
    snprintf(buf, 17, "%016llx", (unsigned long long)v);
}

/* Both cases are read, here and so in ids and double bits: one hex grammar
 * for the three. Only lower case is ever written. */
int gateParseHex64(const char *s, size_t len, uint64_t *out) {
    uint64_t v = 0;
    size_t i;

    if (len != 16) return 0;
    for (i = 0; i < len; i++) {
        char c = s[i];
        unsigned d;

        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a') + 10;
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A') + 10;
        else return 0;
        v = v << 4 | d;
    }
    *out = v;
    return 1;
}

_Static_assert(sizeof(double) == sizeof(uint64_t), "a double is 64 bits");

void gateFormatDoubleBits(char *buf, double v) {
    uint64_t bits;

    memcpy(&bits, &v, sizeof(bits));
    gateFormatHex64(buf, bits);
}

/* The patterns of the infinities and of the NaNs are refused: this file
 * hands no caller a number that is not finite, whether it was typed as a
 * decimal or replicated as bits. A primary only sends values it checked, so
 * such a pattern means a damaged stream, and a NaN let into a gate would
 * end up in a waiting request that the codec then refuses to load. */
int gateParseDoubleBits(const char *s, size_t len, double *out) {
    uint64_t bits;
    double v;

    if (!gateParseHex64(s, len, &bits)) return 0;
    memcpy(&v, &bits, sizeof(v));
    if (!isfinite(v)) return 0;
    *out = v;
    return 1;
}

/* Returns the length of the id, or 0 if 'buflen' bytes cannot hold it and
 * its terminator. GATE_ID_MAX bytes hold any id. */
size_t gateFormatId(char *buf, size_t buflen, uint64_t epoch, uint64_t seq) {
    int n = snprintf(buf, buflen, "%016llx-%llu", (unsigned long long)epoch,
                     (unsigned long long)seq);

    if (n > 0 && (size_t)n < buflen) return (size_t)n;
    if (buflen) buf[0] = '\0'; /* a cut id could still parse, as another one */
    return 0;
}

int gateParseId(const char *s, size_t len, uint64_t *epoch, uint64_t *seq) {
    uint64_t e, q;

    /* A seq that starts with '0' either has a leading zero or is 0. */
    if (len < 18 || s[16] != '-' || s[17] == '0') return 0;
    if (!gateParseHex64(s, 16, &e) || !gateParseU64(s + 17, len - 17, &q)) return 0;
    *epoch = e;
    *seq = q;
    return 1;
}

/* -------------------------------- GCREATE -------------------------------- */

/* The options, in the order of their words below: three that take a number,
 * one that takes nothing, three that take a counted list. */
enum {
    GATE_OPT_ALPHA, GATE_OPT_DELTA, GATE_OPT_AUDIT, GATE_OPT_SCORE,
    GATE_OPT_CLASSES, GATE_OPT_ALWAYSDEFER, GATE_OPT_CLASSALPHA, GATE_OPT_COUNT
};

static const char *const gateOptWord[GATE_OPT_COUNT] = {
    "ALPHA", "DELTA", "AUDIT", "SCORE", "CLASSES", "ALWAYSDEFER", "CLASSALPHA"
};

/* What one command gave: which options and, for a counted list, its first
 * item and its count. */
typedef struct gateCreateOpts {
    int seen[GATE_OPT_COUNT];
    const gateArg *item[GATE_OPT_COUNT];
    int n[GATE_OPT_COUNT];
} gateCreateOpts;

/* Read the command once: numbers go straight into 'cfg', lists are only
 * located. */
static int gateCreateWalk(const gateArg *argv, int argc, gateConfig *cfg,
                          gateCreateOpts *opts, char *err, size_t errlen)
{
    double *number[] = {&cfg->alpha, &cfg->delta, &cfg->audit};
    int i = 0;

    while (i < argc) {
        const gateArg *word = &argv[i++];
        const char *name;
        uint64_t n, per;
        int o = 0;

        while (o < GATE_OPT_COUNT && !gateArgIs(word, gateOptWord[o])) o++;
        if (o == GATE_OPT_COUNT) GATE_ARGS_FAIL("unknown option '%.*s'", GATE_SHOWN(word));
        name = gateOptWord[o];
        if (opts->seen[o]) GATE_ARGS_FAIL("%s given twice", name);
        opts->seen[o] = 1;
        if (o == GATE_OPT_SCORE) continue;
        if (i == argc) GATE_ARGS_FAIL("%s needs an argument", name);

        if (o < GATE_OPT_SCORE) {
            if (!gateParseDouble(argv[i].ptr, argv[i].len, number[o]))
                GATE_ARGS_FAIL("%s must be a number", name);
            i++;
            continue;
        }

        /* A list is skipped by its count, never by looking at its items:
         * a label may spell an option word. */
        per = (o == GATE_OPT_CLASSALPHA) ? 2 : 1;
        if (!gateParseU64(argv[i].ptr, argv[i].len, &n) || n == 0)
            GATE_ARGS_FAIL("%s count must be a positive integer", name);
        i++;
        if (n > (uint64_t)(argc - i) / per)
            GATE_ARGS_FAIL("%s count exceeds the arguments that follow", name);
        opts->item[o] = &argv[i];
        opts->n[o] = (int)n;
        i += (int)(n * per);
    }
    return GATE_OK;
}

/* The rules that need the whole command: what is required, and the two
 * lists that name labels of CLASSES, wherever CLASSES stood. */
static int gateCreateResolve(const gateCreateOpts *opts, gateConfig *cfg,
                             char *err, size_t errlen)
{
    const char *ptr[GATE_MAX_CLASSES];
    size_t len[GATE_MAX_CLASSES];
    char named[GATE_MAX_CLASSES] = {0}; /* labels CLASSALPHA has set */
    const gateArg *a;
    int i, c, n = opts->n[GATE_OPT_CLASSES];

    if (!opts->seen[GATE_OPT_ALPHA]) GATE_ARGS_FAIL("ALPHA is required");
    if (!opts->seen[GATE_OPT_CLASSES]) {
        if (opts->seen[GATE_OPT_ALWAYSDEFER]) GATE_ARGS_FAIL("ALWAYSDEFER needs CLASSES");
        if (opts->seen[GATE_OPT_CLASSALPHA]) GATE_ARGS_FAIL("CLASSALPHA needs CLASSES");
    }
    if (opts->seen[GATE_OPT_CLASSES] == opts->seen[GATE_OPT_SCORE])
        GATE_ARGS_FAIL("exactly one of CLASSES and SCORE is required");
    if (opts->seen[GATE_OPT_SCORE]) return GATE_OK;

    /* Checked before anything is copied: the arrays above hold no more. */
    if (n < GATE_MIN_CLASSES || n > GATE_MAX_CLASSES)
        GATE_ARGS_FAIL("CLASSES must list between %d and %d labels",
                       GATE_MIN_CLASSES, GATE_MAX_CLASSES);
    for (i = 0; i < n; i++) {
        a = &opts->item[GATE_OPT_CLASSES][i];
        /* The config keeps labels as C strings. One with a NUL inside
         * would be cut there and pass gateConfigValidate() as its prefix,
         * so labels are checked while their length is still known. */
        if (!gateValidName(a->ptr, a->len, GATE_MAX_LABEL_BYTES))
            GATE_ARGS_FAIL("labels must be 1 to %u bytes of UTF-8 without "
                           "control characters", GATE_MAX_LABEL_BYTES);
        ptr[i] = a->ptr;
        len[i] = a->len;
    }
    cfg->kind = GATE_KIND_CLASSIFIER;
    if (gateConfigSetClasses(cfg, ptr, len, (uint32_t)n) != GATE_OK)
        GATE_ARGS_FAIL("out of memory");

    for (i = 0; i < opts->n[GATE_OPT_ALWAYSDEFER]; i++) {
        a = &opts->item[GATE_OPT_ALWAYSDEFER][i];
        c = gateConfigLabelIndex(cfg, a->ptr, a->len);
        if (c < 0) GATE_ARGS_FAIL("unknown label '%.*s' in ALWAYSDEFER", GATE_SHOWN(a));
        if (cfg->class_defer[c])
            GATE_ARGS_FAIL("label '%s' given twice in ALWAYSDEFER", cfg->labels[c]);
        cfg->class_defer[c] = 1;
    }
    for (i = 0; i < opts->n[GATE_OPT_CLASSALPHA]; i++) {
        double alpha;

        a = &opts->item[GATE_OPT_CLASSALPHA][2 * i];
        c = gateConfigLabelIndex(cfg, a->ptr, a->len);
        if (c < 0) GATE_ARGS_FAIL("unknown label '%.*s' in CLASSALPHA", GATE_SHOWN(a));
        if (named[c])
            GATE_ARGS_FAIL("label '%s' given twice in CLASSALPHA", cfg->labels[c]);
        if (cfg->class_defer[c])
            GATE_ARGS_FAIL("label '%s' is in both ALWAYSDEFER and CLASSALPHA", cfg->labels[c]);
        /* gateConfigValidate() checks the upper bound, but it reads 0 as
         * "not set" and would turn it into ALPHA: 0 is refused here. */
        if (!gateParseDouble(a[1].ptr, a[1].len, &alpha) || !(alpha > 0.0))
            GATE_ARGS_FAIL("CLASSALPHA for '%s' must be greater than 0 and at most ALPHA",
                           cfg->labels[c]);
        cfg->class_alpha[c] = alpha;
        named[c] = 1;
    }
    return GATE_OK;
}

int gateParseCreate(const gateArg *argv, int argc, uint32_t snapshot_examples,
                    int64_t feedback_timeout_ms, gateConfig *cfg,
                    char *err, size_t errlen)
{
    gateCreateOpts opts;

    memset(&opts, 0, sizeof(opts));
    gateConfigInit(cfg);
    cfg->snapshot_examples = snapshot_examples;
    cfg->feedback_timeout_ms = feedback_timeout_ms;
    if (gateCreateWalk(argv, argc, cfg, &opts, err, errlen) == GATE_OK &&
        gateCreateResolve(&opts, cfg, err, errlen) == GATE_OK &&
        gateConfigValidate(cfg, err, errlen) == GATE_OK)
        return GATE_OK;
    gateConfigFree(cfg); /* the one failure exit: nothing leaks, cfg is zeroed */
    return GATE_ERR;
}

/* ------------------------------- Requests -------------------------------- */

int gateParseRequest(const gateArg *argv, int argc, int *pos, gateRequest *req,
                     char *err, size_t errlen)
{
    const gateArg *text;
    uint64_t n = 0, k;
    int i = *pos;

    req->text = NULL; /* empty unless the whole request parses */
    req->text_len = 0;
    req->nfeatures = 0;

    if (argc - i < 2 || !gateArgIs(&argv[i], "TEXT")) GATE_ARGS_FAIL("expected TEXT");
    text = &argv[i + 1];
    if (text->len > GATE_MAX_TEXT_BYTES)
        GATE_ARGS_FAIL("TEXT is longer than %u bytes", GATE_MAX_TEXT_BYTES);
    i += 2;

    if (i < argc && gateArgIs(&argv[i], "FEATURES")) {
        if (++i == argc || !gateParseU64(argv[i].ptr, argv[i].len, &n) ||
            n > GATE_MAX_FEATURES)
            GATE_ARGS_FAIL("FEATURES count must be an integer between 0 and %u",
                           GATE_MAX_FEATURES);
        i++;
        if (n > (uint64_t)(argc - i) / 2)
            GATE_ARGS_FAIL("FEATURES count exceeds the arguments that follow");
        for (k = 0; k < n; k++, i += 2) {
            const gateArg *name = &argv[i], *value = &argv[i + 1];
            gateNamedFeature *f = &req->features[k];

            if (!gateValidName(name->ptr, name->len, GATE_MAX_NAME_BYTES))
                GATE_ARGS_FAIL("feature names must be 1 to %u bytes of UTF-8 without "
                               "control characters", GATE_MAX_NAME_BYTES);
            if (!gateParseDouble(value->ptr, value->len, &f->value))
                GATE_ARGS_FAIL("the value of feature '%.*s' must be a finite number",
                               (int)name->len, name->ptr);
            f->name = name->ptr;
            f->name_len = name->len;
        }
    }
    req->text = text->ptr;
    req->text_len = text->len;
    req->nfeatures = (uint32_t)n;
    *pos = i;
    return GATE_OK;
}

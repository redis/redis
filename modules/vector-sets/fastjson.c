/* Ultra‑lightweight top‑level JSON field extractor.
 * Return the element directly as an expr.c token.
 * This code is directly included inside expr.c.
 *
 * Copyright (c) 2025-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2) or the Server Side Public License v1 (SSPLv1).
 *
 * Originally authored by: Salvatore Sanfilippo.
 *
 * ------------------------------------------------------------------
 *
 * DESIGN GOALS:
 *
 * 1. Zero heap allocations while seeking the requested key.
 * 2. A single parse (and therefore a single allocation, if needed)
 *    when the key finally matches.
 * 3. Same subset‑of‑JSON coverage needed by expr.c:
 * - Strings (escapes: \" \\ \n \r \t).
 * - Numbers (double).
 * - Booleans.
 * - Null.
 * - Flat arrays of the above primitives.
 *
 * Any other value (nested object, unicode escape, etc.) returns NULL.
 * Should be very easy to extend it in case in the future we want
 * more for the FILTER option of VSIM.
 * 4. No global state, so this file can be #included directly in expr.c.
 *
 * The only API expr.c uses directly is:
 *
 * exprtoken *jsonExtractField(const char *json, size_t json_len,
 * const char *field, size_t field_len);
 * ------------------------------------------------------------------ */

#include <ctype.h>
#include <string.h>

// Forward declarations.
static int jsonSkipValue(const char **p, const char *end);
static exprtoken *jsonParseValueToken(const char **p, const char *end);

/* Similar to ctype.h isdigit() but covers the whole JSON number charset,
 * including exp form. */
static int jsonIsNumberChar(int c) {
    return isdigit(c) || c=='-' || c=='+' || c=='.' || c=='e' || c=='E';
}

/* ========================== Fast skipping of JSON =========================
 * The helpers here are designed to skip values without performing any
 * allocation. This way, for the use case of this JSON parser, we are able
 * to easily (and with good speed) skip fields and values we are not
 * interested in. Then, later in the code, when we find the field we want
 * to obtain, we finally call the functions that turn a given JSON value
 * associated to a field into our of our expressions token.
 * ========================================================================== */

/* Advance *p consuming all the spaces. */
static inline void jsonSkipWhiteSpaces(const char **p, const char *end) {
    while (*p < end && isspace((unsigned char)**p)) (*p)++;
}

/* Advance *p past a JSON string. Returns 1 on success, 0 on error. */
static int jsonSkipString(const char **p, const char *end) {
    if (*p >= end || **p != '"') return 0;
    (*p)++; /* Skip opening quote. */
    while (*p < end) {
        if (**p == '\\') {
            (*p) += 2;
            continue;
        }
        if (**p == '"') {
            (*p)++; /* Skip closing quote. */
            return 1;
        }
        (*p)++;
    }
    return 0; /* unterminated */
}

/* Skip an array or object generically using depth counter.
 * Opener and closer tells the function how the aggregated
 * data type starts/stops, basically [] or {}. */
static int jsonSkipBracketed(const char **p, const char *end,
                             char opener, char closer) {
    int depth = 1;
    (*p)++; /* Skip opener. */

    /* Loop until we reach the end of the input or find the matching
     * closer (depth becomes 0). */
    while (*p < end && depth > 0) {
        char c = **p;

        if (c == '"') {
            // Found a string, delegate skipping to jsonSkipString().
            if (!jsonSkipString(p, end)) {
                return 0; // String skipping failed (e.g., unterminated)
            }
            /* jsonSkipString() advances *p past the closing quote.
             * Continue the loop to process the character *after* the string. */
            continue;
        }

        /* If it's not a string, check if it affects the depth for the
         * specific brackets we are currently tracking. */
        if (c == opener) {
            depth++;
        } else if (c == closer) {
            depth--;
        }

        /* Always advance the pointer for any non-string character.
         * This handles commas, colons, whitespace, numbers, literals,
         * and even nested brackets of a *different* type than the
         * one we are currently skipping (e.g. skipping a { inside []). */
        (*p)++;
    }

    /* Return 1 (true) if we successfully found the matching closer,
     * otherwise there is a parse error and we return 0. */
    return depth == 0;
}

/* Skip a single JSON literal (true, null, ...) starting at *p.
 * Returns 1 on success, 0 on failure. */
static int jsonSkipLiteral(const char **p, const char *end, const char *lit) {
    size_t l = strlen(lit);
    if (*p + l > end) return 0;
    if (strncmp(*p, lit, l) == 0) { *p += l; return 1; }
    return 0;
}

/* Skip number, don't check that number format is correct, just consume
 * number-alike characters.
 *
 * Note: More robust number skipping might check validity,
 * but for skipping, just consuming plausible characters is enough. */
static int jsonSkipNumber(const char **p, const char *end) {
    const char *num_start = *p;
    while (*p < end && jsonIsNumberChar(**p)) (*p)++;
    return *p > num_start; // Any progress made? Otherwise no number found.
}

/* Skip any JSON value. 1 = success, 0 = error. */
static int jsonSkipValue(const char **p, const char *end) {
    jsonSkipWhiteSpaces(p, end);
    if (*p >= end) return 0;
    switch (**p) {
    case '"': return jsonSkipString(p, end);
    case '{':  return jsonSkipBracketed(p, end, '{', '}');
    case '[':  return jsonSkipBracketed(p, end, '[', ']');
    case 't':  return jsonSkipLiteral(p, end, "true");
    case 'f':  return jsonSkipLiteral(p, end, "false");
    case 'n':  return jsonSkipLiteral(p, end, "null");
    default: return jsonSkipNumber(p, end);
    }
}

/* =========================== JSON to exprtoken ============================
 * The functions below convert a given json value to the equivalent
 * expression token structure.
 * ========================================================================== */

static exprtoken *jsonParseStringToken(const char **p, const char *end) {
    if (*p >= end || **p != '"') return NULL;
    const char *start = ++(*p);
    int esc = 0; size_t len = 0; int has_esc = 0;
    const char *q = *p;
    while (q < end) {
        if (esc) { esc = 0; q++; len++; has_esc = 1; continue; }
        if (*q == '\\') { esc = 1; q++; continue; }
        if (*q == '"') break;
        q++; len++;
    }
    if (q >= end || *q != '"') return NULL; // Unterminated string
    exprtoken *t = exprNewToken(EXPR_TOKEN_STR);

    if (!has_esc) {
        // No escapes, we can point directly into the original JSON string.
        t->str.start = (char*)start; t->str.len = len; t->str.heapstr = NULL;
    } else {
        // Escapes present, need to allocate and copy/process escapes.
        char *dst = RedisModule_Alloc(len + 1);

        t->str.start = t->str.heapstr = dst; t->str.len = len;
        const char *r = start; esc = 0;
        while (r < q) {
            if (esc) {
                switch (*r) {
                // Supported escapes from Goal 3.
                case 'n': *dst='\n'; break;
                case 'r': *dst='\r'; break;
                case 't': *dst='\t'; break;
                case '\\': *dst='\\'; break;
                case '"': *dst='\"'; break;
                // Escapes (like \uXXXX, \b, \f) are not supported for now,
                // we just copy them verbatim.
                default: *dst=*r; break;
                }
                dst++; esc = 0; r++; continue;
            }
            if (*r == '\\') { esc = 1; r++; continue; }
            *dst++ = *r++;
        }
        *dst = '\0'; // Null-terminate the allocated string.
    }
    *p = q + 1; // Advance the main pointer past the closing quote.
    return t;
}

static exprtoken *jsonParseNumberToken(const char **p, const char *end) {
    // Use a buffer to extract the number literal for parsing with strtod().
    char buf[256]; int idx = 0;
    const char *start = *p; // For strtod partial failures check.

    // Copy potential number characters to buffer.
    while (*p < end && idx < (int)sizeof(buf)-1 && jsonIsNumberChar(**p)) {
        buf[idx++] = **p;
        (*p)++;
    }
    buf[idx]='\0'; // Null-terminate buffer.

    if (idx==0) return NULL; // No number characters found.

    char *ep; // End pointer for strtod validation.
    double v = strtod(buf, &ep);

    /* Check if strtod() consumed the entire buffer content.
     * If not, the number format was invalid. */
    if (*ep!='\0') {
        // strtod() failed; rewind p to the start and return NULL
        *p = start;
        return NULL;
    }

    // If strtod() succeeded, create and return the token..
    exprtoken *t = exprNewToken(EXPR_TOKEN_NUM);
    t->num = v;
    return t;
}

static exprtoken *jsonParseLiteralToken(const char **p, const char *end, const char *lit, int type, double num) {
    size_t l = strlen(lit);

    // Ensure we don't read past 'end'.
    if ((*p + l) > end) return NULL;

    if (strncmp(*p, lit, l) != 0) return NULL; // Literal doesn't match.

    // Check that the character *after* the literal is a valid JSON delimiter
    // (whitespace, comma, closing bracket/brace, or end of input)
    // This prevents matching "trueblabla" as "true".
    if ((*p + l) < end) {
        char next_char = *(*p + l);
        if (!isspace((unsigned char)next_char) && next_char!=',' &&
            next_char!=']' && next_char!='}') {
            return NULL; // Invalid character following literal.
        }
    }

    // Literal matched and is correctly terminated.
    *p += l;
    exprtoken *t = exprNewToken(type);
    t->num = num;
    return t;
}

static exprtoken *jsonParseArrayToken(const char **p, const char *end) {
    if (*p >= end || **p != '[') return NULL;
    (*p)++; // Skip '['.
    jsonSkipWhiteSpaces(p,end);

    exprtoken *t = exprNewToken(EXPR_TOKEN_TUPLE);
    t->tuple.len = 0; t->tuple.ele = NULL; size_t alloc = 0;

    // Handle empty array [].
    if (*p < end && **p == ']') {
        (*p)++; // Skip ']'.
        return t;
    }

    // Parse array elements.
    while (1) {
        exprtoken *ele = jsonParseValueToken(p,end);
        if (!ele) {
            exprTokenRelease(t); // Clean up partially built array token.
            return NULL;
        }

        // Grow allocated space for elements if needed.
        if (t->tuple.len == alloc) {
            size_t newsize = alloc ? alloc * 2 : 4;
            // Check for potential overflow if newsize becomes huge.
            if (newsize < alloc) {
                exprTokenRelease(ele);
                exprTokenRelease(t);
                return NULL;
            }
            exprtoken **newele = RedisModule_Realloc(t->tuple.ele,
                                           sizeof(exprtoken*)*newsize);
            t->tuple.ele = newele;
            alloc = newsize;
        }
        t->tuple.ele[t->tuple.len++] = ele; // Add element.

        jsonSkipWhiteSpaces(p,end);
        if (*p>=end) {
            // Unterminated array. Note that this check is crucial because
            // previous value parsed may seek 'p' to 'end'.
            exprTokenRelease(t);
            return NULL;
        }

        // Check for comma (more elements) or closing bracket.
        if (**p == ',') {
            (*p)++; // Skip ','
            jsonSkipWhiteSpaces(p,end); // Skip whitespace before next element
            continue; // Parse next element
        } else if (**p == ']') {
            (*p)++; // Skip ']'
            return t; // End of array
        } else {
            // Unexpected character (not ',' or ']')
            exprTokenRelease(t);
            return NULL;
        }
    }
}

/* Turn a JSON value into an expr token. */
static exprtoken *jsonParseValueToken(const char **p, const char *end) {
    jsonSkipWhiteSpaces(p,end);
    if (*p >= end) return NULL;

    switch (**p) {
    case '"': return jsonParseStringToken(p,end);
    case '[':  return jsonParseArrayToken(p,end);
    case '{':  return NULL; // No nested elements support for now.
    case 't':  return jsonParseLiteralToken(p,end,"true",EXPR_TOKEN_NUM,1);
    case 'f':  return jsonParseLiteralToken(p,end,"false",EXPR_TOKEN_NUM,0);
    case 'n':  return jsonParseLiteralToken(p,end,"null",EXPR_TOKEN_NULL,0);
    default:
        // Check if it starts like a number.
        if (isdigit((unsigned char)**p) || **p=='-' || **p=='+') {
             return jsonParseNumberToken(p,end);
        }
        // Anything else is an unsupported type or malformed JSON.
        return NULL;
    }
}

/* ============================== Fast key seeking ========================== */

/* Finds the start of the value for a given field key within a JSON object.
 * Returns pointer to the first char of the value, or NULL if not found/error.
 * This function does not perform any allocation and is optimized to seek
 * the specified *toplevel* filed as fast as possible. */
static const char *jsonSeekField(const char *json, const char *end,
                                 const char *field, size_t flen) {
    const char *p = json;
    jsonSkipWhiteSpaces(&p,end);
    if (p >= end || *p != '{') return NULL; // Must start with '{'.
    p++; // skip '{'.

    while (1) {
        jsonSkipWhiteSpaces(&p,end);
        if (p >= end) return NULL; // Reached end within object.

        if (*p == '}') return NULL; // End of object, field not found.

        // Expecting a key (string).
        if (*p != '"') return NULL; // Key must be a string.

        // --- Key Matching using jsonSkipString ---
        const char *key_start = p + 1; // Start of key content.
        const char *key_end_p = p;     // Will later contain the end.

        // Use jsonSkipString() to find the end.
        if (!jsonSkipString(&key_end_p, end)) {
            // Unterminated / invalid key string.
            return NULL;
        }

        // Calculate the length of the key's content.
        size_t klen = (key_end_p - 1) - key_start;

        /* Perform the comparison using the raw key content.
         * WARNING: This uses memcmp(), so we don't handle escaped chars
         * within the key matching against unescaped chars in 'field'. */
        int match = klen == flen && !memcmp(key_start, field, flen);

        // Update the main pointer 'p' to be after the key string.
        p = key_end_p;

        // Now we expect to find a ":" followed by a value.
        jsonSkipWhiteSpaces(&p,end);
        if (p>=end || *p!=':') return NULL; // Expect ':' after key
        p++; // Skip ':'.

	// Seek value.
        jsonSkipWhiteSpaces(&p,end);
        if (p>=end) return NULL; // Expect value after ':'

        if (match) {
            // Found the matching key, p now points to the start of the value.
            return p;
        } else {
            // Key didn't match, skip the corresponding value.
            if (!jsonSkipValue(&p,end)) return NULL; // Syntax error.
        }


        // Look for comma or a closing brace.
        jsonSkipWhiteSpaces(&p,end);
        if (p>=end) return NULL; // Reached end after value.

        if (*p == ',') {
            p++; // Skip comma, continue loop to find next key.
            continue;
        } else if (*p == '}') {
            return NULL; // Reached end of object, field not found.
        }
        return NULL; // Malformed JSON (unexpected char after value).
    }
}

/* This is the only real API that this file conceptually exports (it is
 * inlined, actually). */
exprtoken *jsonExtractField(const char *json, size_t json_len,
                            const char *field, size_t field_len)
{
    const char *end = json + json_len;
    const char *valptr = jsonSeekField(json,end,field,field_len);
    if (!valptr) return NULL;

    /* Key found, valptr points to the start of the value.
     * Convert it into an expression token object. */
    return jsonParseValueToken(&valptr,end);
}

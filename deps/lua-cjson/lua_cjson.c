/* CJSON - JSON support for Lua
 *
 * Copyright (c) 2010-2011  Mark Pulford <mark@kyne.com.au>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/* Caveats:
 * - JSON "null" values are represented as lightuserdata since Lua
 *   tables cannot contain "nil". Compare with cjson.null.
 * - Invalid UTF-8 characters are not detected and will be passed
 *   untouched.
 * - Javascript comments are not part of the JSON spec, and are not
 *   supported.
 *
 * Note: Decoding is slower than encoding. Lua spends significant
 *       time (30%) managing tables when parsing JSON since it is
 *       difficult to know object/array sizes ahead of time.
 */

#include <assert.h>
#include <string.h>
#include <math.h>
#include <lua.h>
#include <lauxlib.h>

#include "strbuf.h"

#define DEFAULT_SPARSE_CONVERT 0
#define DEFAULT_SPARSE_RATIO 2
#define DEFAULT_SPARSE_SAFE 10
#define DEFAULT_MAX_DEPTH 20
#define DEFAULT_ENCODE_REFUSE_BADNUM 1
#define DEFAULT_DECODE_REFUSE_BADNUM 0
#define DEFAULT_ENCODE_KEEP_BUFFER 1

typedef enum {
    T_OBJ_BEGIN,
    T_OBJ_END,
    T_ARR_BEGIN,
    T_ARR_END,
    T_STRING,
    T_NUMBER,
    T_BOOLEAN,
    T_NULL,
    T_COLON,
    T_COMMA,
    T_END,
    T_WHITESPACE,
    T_ERROR,
    T_UNKNOWN
} json_token_type_t;

static const char *json_token_type_name[] = {
    "T_OBJ_BEGIN",
    "T_OBJ_END",
    "T_ARR_BEGIN",
    "T_ARR_END",
    "T_STRING",
    "T_NUMBER",
    "T_BOOLEAN",
    "T_NULL",
    "T_COLON",
    "T_COMMA",
    "T_END",
    "T_WHITESPACE",
    "T_ERROR",
    "T_UNKNOWN",
    NULL
};

typedef struct {
    json_token_type_t ch2token[256];
    char escape2char[256];  /* Decoding */
#if 0
    char escapes[35][8];    /* Pre-generated escape string buffer */
    char *char2escape[256]; /* Encoding */
#endif
    strbuf_t encode_buf;
    int current_depth;

    int encode_sparse_convert;
    int encode_sparse_ratio;
    int encode_sparse_safe;
    int encode_max_depth;
    int encode_refuse_badnum;
    int decode_refuse_badnum;
    int encode_keep_buffer;
} json_config_t;

typedef struct {
    const char *data;
    int index;
    strbuf_t *tmp;    /* Temporary storage for strings */
    json_config_t *cfg;
} json_parse_t;

typedef struct {
    json_token_type_t type;
    int index;
    union {
        const char *string;
        double number;
        int boolean;
    } value;
    int string_len;
} json_token_t;

static const char *char2escape[256] = {
    "\\u0000", "\\u0001", "\\u0002", "\\u0003",
    "\\u0004", "\\u0005", "\\u0006", "\\u0007",
    "\\b", "\\t", "\\n", "\\u000b",
    "\\f", "\\r", "\\u000e", "\\u000f",
    "\\u0010", "\\u0011", "\\u0012", "\\u0013",
    "\\u0014", "\\u0015", "\\u0016", "\\u0017",
    "\\u0018", "\\u0019", "\\u001a", "\\u001b",
    "\\u001c", "\\u001d", "\\u001e", "\\u001f",
    NULL, NULL, "\\\"", NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, "\\/",
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, "\\\\", NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, "\\u007f",
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
};

static int json_config_key;

/* ===== CONFIGURATION ===== */

static json_config_t *json_fetch_config(lua_State *l)
{
    json_config_t *cfg;

    lua_pushlightuserdata(l, &json_config_key);
    lua_gettable(l, LUA_REGISTRYINDEX);
    cfg = lua_touserdata(l, -1);
    if (!cfg)
        luaL_error(l, "BUG: Unable to fetch CJSON configuration");

    lua_pop(l, 1);

    return cfg;
}

static void json_verify_arg_count(lua_State *l, int args)
{
    luaL_argcheck(l, lua_gettop(l) <= args, args + 1,
                  "found too many arguments");
}

/* Configures handling of extremely sparse arrays:
 * convert: Convert extremely sparse arrays into objects? Otherwise error.
 * ratio: 0: always allow sparse; 1: never allow sparse; >1: use ratio
 * safe: Always use an array when the max index <= safe */
static int json_cfg_encode_sparse_array(lua_State *l)
{
    json_config_t *cfg;
    int val;

    json_verify_arg_count(l, 3);
    cfg = json_fetch_config(l);

    switch (lua_gettop(l)) {
    case 3:
        val = luaL_checkinteger(l, 3);
        luaL_argcheck(l, val >= 0, 3, "expected integer >= 0");
        cfg->encode_sparse_safe = val;
    case 2:
        val = luaL_checkinteger(l, 2);
        luaL_argcheck(l, val >= 0, 2, "expected integer >= 0");
        cfg->encode_sparse_ratio = val;
    case 1:
        luaL_argcheck(l, lua_isboolean(l, 1), 1, "expected boolean");
        cfg->encode_sparse_convert = lua_toboolean(l, 1);
    }

    lua_pushboolean(l, cfg->encode_sparse_convert);
    lua_pushinteger(l, cfg->encode_sparse_ratio);
    lua_pushinteger(l, cfg->encode_sparse_safe);

    return 3;
}

/* Configures the maximum number of nested arrays/objects allowed when
 * encoding */
static int json_cfg_encode_max_depth(lua_State *l)
{
    json_config_t *cfg;
    int depth;

    json_verify_arg_count(l, 1);
    cfg = json_fetch_config(l);

    if (lua_gettop(l)) {
        depth = luaL_checkinteger(l, 1);
        luaL_argcheck(l, depth > 0, 1, "expected positive integer");
        cfg->encode_max_depth = depth;
    }

    lua_pushinteger(l, cfg->encode_max_depth);

    return 1;
}

/* Configures JSON encoding buffer persistence */
static int json_cfg_encode_keep_buffer(lua_State *l)
{
    json_config_t *cfg;

    json_verify_arg_count(l, 1);
    cfg = json_fetch_config(l);

    if (lua_gettop(l)) {
        luaL_checktype(l, 1, LUA_TBOOLEAN);
        cfg->encode_keep_buffer = lua_toboolean(l, 1);
    }

    lua_pushboolean(l, cfg->encode_keep_buffer);

    return 1;
}

/* On argument: decode enum and set config variables
 * **options must point to a NULL terminated array of 4 enums
 * Returns: current enum value */
static void json_enum_option(lua_State *l, const char **options,
                             int *opt1, int *opt2)
{
    int setting;

    if (lua_gettop(l)) {
        if (lua_isboolean(l, 1))
            setting = lua_toboolean(l, 1) * 3;
        else
            setting = luaL_checkoption(l, 1, NULL, options);

        *opt1 = setting & 1 ? 1 : 0;
        *opt2 = setting & 2 ? 1 : 0;
    } else {
        setting = *opt1 | (*opt2 << 1);
    }

    if (setting)
        lua_pushstring(l, options[setting]);
    else
        lua_pushboolean(l, 0);
}


/* When enabled, rejects: NaN, Infinity, hexidecimal numbers */
static int json_cfg_refuse_invalid_numbers(lua_State *l)
{
    static const char *options_enc_dec[] = { "none", "encode", "decode",
                                             "both", NULL };
    json_config_t *cfg;

    json_verify_arg_count(l, 1);
    cfg = json_fetch_config(l);

    json_enum_option(l, options_enc_dec,
                     &cfg->encode_refuse_badnum,
                     &cfg->decode_refuse_badnum);

    return 1;
}

static int json_destroy_config(lua_State *l)
{
    json_config_t *cfg;

    cfg = lua_touserdata(l, 1);
    if (cfg)
        strbuf_free(&cfg->encode_buf);
    cfg = NULL;

    return 0;
}

static void json_create_config(lua_State *l)
{
    json_config_t *cfg;
    int i;

    cfg = lua_newuserdata(l, sizeof(*cfg));

    /* Create GC method to clean up strbuf */
    lua_newtable(l);
    lua_pushcfunction(l, json_destroy_config);
    lua_setfield(l, -2, "__gc");
    lua_setmetatable(l, -2);

    strbuf_init(&cfg->encode_buf, 0);

    cfg->encode_sparse_convert = DEFAULT_SPARSE_CONVERT;
    cfg->encode_sparse_ratio = DEFAULT_SPARSE_RATIO;
    cfg->encode_sparse_safe = DEFAULT_SPARSE_SAFE;
    cfg->encode_max_depth = DEFAULT_MAX_DEPTH;
    cfg->encode_refuse_badnum = DEFAULT_ENCODE_REFUSE_BADNUM;
    cfg->decode_refuse_badnum = DEFAULT_DECODE_REFUSE_BADNUM;
    cfg->encode_keep_buffer = DEFAULT_ENCODE_KEEP_BUFFER;

    /* Decoding init */

    /* Tag all characters as an error */
    for (i = 0; i < 256; i++)
        cfg->ch2token[i] = T_ERROR;

    /* Set tokens that require no further processing */
    cfg->ch2token['{'] = T_OBJ_BEGIN;
    cfg->ch2token['}'] = T_OBJ_END;
    cfg->ch2token['['] = T_ARR_BEGIN;
    cfg->ch2token[']'] = T_ARR_END;
    cfg->ch2token[','] = T_COMMA;
    cfg->ch2token[':'] = T_COLON;
    cfg->ch2token['\0'] = T_END;
    cfg->ch2token[' '] = T_WHITESPACE;
    cfg->ch2token['\t'] = T_WHITESPACE;
    cfg->ch2token['\n'] = T_WHITESPACE;
    cfg->ch2token['\r'] = T_WHITESPACE;

    /* Update characters that require further processing */
    cfg->ch2token['f'] = T_UNKNOWN;     /* false? */
    cfg->ch2token['i'] = T_UNKNOWN;     /* inf, ininity? */
    cfg->ch2token['I'] = T_UNKNOWN;
    cfg->ch2token['n'] = T_UNKNOWN;     /* null, nan? */
    cfg->ch2token['N'] = T_UNKNOWN;
    cfg->ch2token['t'] = T_UNKNOWN;     /* true? */
    cfg->ch2token['"'] = T_UNKNOWN;     /* string? */
    cfg->ch2token['+'] = T_UNKNOWN;     /* number? */
    cfg->ch2token['-'] = T_UNKNOWN;
    for (i = 0; i < 10; i++)
        cfg->ch2token['0' + i] = T_UNKNOWN;

    /* Lookup table for parsing escape characters */
    for (i = 0; i < 256; i++)
        cfg->escape2char[i] = 0;          /* String error */
    cfg->escape2char['"'] = '"';
    cfg->escape2char['\\'] = '\\';
    cfg->escape2char['/'] = '/';
    cfg->escape2char['b'] = '\b';
    cfg->escape2char['t'] = '\t';
    cfg->escape2char['n'] = '\n';
    cfg->escape2char['f'] = '\f';
    cfg->escape2char['r'] = '\r';
    cfg->escape2char['u'] = 'u';          /* Unicode parsing required */


#if 0
    /* Initialise separate storage for pre-generated escape codes.
     * Escapes 0-31 map directly, 34, 92, 127 follow afterwards to
     * save memory. */
    for (i = 0 ; i < 32; i++)
        sprintf(cfg->escapes[i], "\\u%04x", i);
    strcpy(cfg->escapes[8], "\b");              /* Override simpler escapes */
    strcpy(cfg->escapes[9], "\t");
    strcpy(cfg->escapes[10], "\n");
    strcpy(cfg->escapes[12], "\f");
    strcpy(cfg->escapes[13], "\r");
    strcpy(cfg->escapes[32], "\\\"");           /* chr(34) */
    strcpy(cfg->escapes[33], "\\\\");           /* chr(92) */
    sprintf(cfg->escapes[34], "\\u%04x", 127);  /* char(127) */

    /* Initialise encoding escape lookup table */
    for (i = 0; i < 32; i++)
        cfg->char2escape[i] = cfg->escapes[i];
    for (i = 32; i < 256; i++)
        cfg->char2escape[i] = NULL;
    cfg->char2escape[34] = cfg->escapes[32];
    cfg->char2escape[92] = cfg->escapes[33];
    cfg->char2escape[127] = cfg->escapes[34];
#endif
}

/* ===== ENCODING ===== */

static void json_encode_exception(lua_State *l, json_config_t *cfg, int lindex,
                                  const char *reason)
{
    if (!cfg->encode_keep_buffer)
        strbuf_free(&cfg->encode_buf);
    luaL_error(l, "Cannot serialise %s: %s",
                  lua_typename(l, lua_type(l, lindex)), reason);
}

/* json_append_string args:
 * - lua_State
 * - JSON strbuf
 * - String (Lua stack index)
 *
 * Returns nothing. Doesn't remove string from Lua stack */
static void json_append_string(lua_State *l, strbuf_t *json, int lindex)
{
    const char *escstr;
    int i;
    const char *str;
    size_t len;

    str = lua_tolstring(l, lindex, &len);

    /* Worst case is len * 6 (all unicode escapes).
     * This buffer is reused constantly for small strings
     * If there are any excess pages, they won't be hit anyway.
     * This gains ~5% speedup. */
    strbuf_ensure_empty_length(json, len * 6 + 2);

    strbuf_append_char_unsafe(json, '\"');
    for (i = 0; i < len; i++) {
        escstr = char2escape[(unsigned char)str[i]];
        if (escstr)
            strbuf_append_string(json, escstr);
        else
            strbuf_append_char_unsafe(json, str[i]);
    }
    strbuf_append_char_unsafe(json, '\"');
}

/* Find the size of the array on the top of the Lua stack
 * -1   object (not a pure array)
 * >=0  elements in array
 */
static int lua_array_length(lua_State *l, json_config_t *cfg)
{
    double k;
    int max;
    int items;

    max = 0;
    items = 0;

    lua_pushnil(l);
    /* table, startkey */
    while (lua_next(l, -2) != 0) {
        /* table, key, value */
        if (lua_isnumber(l, -2) &&
            (k = lua_tonumber(l, -2))) {
            /* Integer >= 1 ? */
            if (floor(k) == k && k >= 1) {
                if (k > max)
                    max = k;
                items++;
                lua_pop(l, 1);
                continue;
            }
        }

        /* Must not be an array (non integer key) */
        lua_pop(l, 2);
        return -1;
    }

    /* Encode very sparse arrays as objects (if enabled) */
    if (cfg->encode_sparse_ratio > 0 &&
        max > items * cfg->encode_sparse_ratio &&
        max > cfg->encode_sparse_safe) {
        if (!cfg->encode_sparse_convert)
            json_encode_exception(l, cfg, -1, "excessively sparse array");

        return -1;
    }

    return max;
}

static void json_encode_descend(lua_State *l, json_config_t *cfg)
{
    cfg->current_depth++;

    if (cfg->current_depth > cfg->encode_max_depth) {
        if (!cfg->encode_keep_buffer)
            strbuf_free(&cfg->encode_buf);
        luaL_error(l, "Cannot serialise, excessive nesting (%d)",
                   cfg->current_depth);
    }
}

static void json_append_data(lua_State *l, json_config_t *cfg, strbuf_t *json);

/* json_append_array args:
 * - lua_State
 * - JSON strbuf
 * - Size of passwd Lua array (top of stack) */
static void json_append_array(lua_State *l, json_config_t *cfg, strbuf_t *json,
                              int array_length)
{
    int comma, i;

    json_encode_descend(l, cfg);

    strbuf_append_char(json, '[');

    comma = 0;
    for (i = 1; i <= array_length; i++) {
        if (comma)
            strbuf_append_char(json, ',');
        else
            comma = 1;

        lua_rawgeti(l, -1, i);
        json_append_data(l, cfg, json);
        lua_pop(l, 1);
    }

    strbuf_append_char(json, ']');

    cfg->current_depth--;
}

static void json_append_number(lua_State *l, strbuf_t *json, int index,
                               json_config_t *cfg)
{
    double num = lua_tonumber(l, index);

    if (cfg->encode_refuse_badnum && (isinf(num) || isnan(num)))
        json_encode_exception(l, cfg, index, "must not be NaN or Inf");

    strbuf_append_number(json, num);
}

static void json_append_object(lua_State *l, json_config_t *cfg,
                               strbuf_t *json)
{
    int comma, keytype;

    json_encode_descend(l, cfg);

    /* Object */
    strbuf_append_char(json, '{');

    lua_pushnil(l);
    /* table, startkey */
    comma = 0;
    while (lua_next(l, -2) != 0) {
        if (comma)
            strbuf_append_char(json, ',');
        else
            comma = 1;

        /* table, key, value */
        keytype = lua_type(l, -2);
        if (keytype == LUA_TNUMBER) {
            strbuf_append_char(json, '"');
            json_append_number(l, json, -2, cfg);
            strbuf_append_mem(json, "\":", 2);
        } else if (keytype == LUA_TSTRING) {
            json_append_string(l, json, -2);
            strbuf_append_char(json, ':');
        } else {
            json_encode_exception(l, cfg, -2,
                                  "table key must be a number or string");
            /* never returns */
        }

        /* table, key, value */
        json_append_data(l, cfg, json);
        lua_pop(l, 1);
        /* table, key */
    }

    strbuf_append_char(json, '}');

    cfg->current_depth--;
}

/* Serialise Lua data into JSON string. */
static void json_append_data(lua_State *l, json_config_t *cfg, strbuf_t *json)
{
    int len;

    switch (lua_type(l, -1)) {
    case LUA_TSTRING:
        json_append_string(l, json, -1);
        break;
    case LUA_TNUMBER:
        json_append_number(l, json, -1, cfg);
        break;
    case LUA_TBOOLEAN:
        if (lua_toboolean(l, -1))
            strbuf_append_mem(json, "true", 4);
        else
            strbuf_append_mem(json, "false", 5);
        break;
    case LUA_TTABLE:
        len = lua_array_length(l, cfg);
        if (len > 0)
            json_append_array(l, cfg, json, len);
        else
            json_append_object(l, cfg, json);
        break;
    case LUA_TNIL:
        strbuf_append_mem(json, "null", 4);
        break;
    case LUA_TLIGHTUSERDATA:
        if (lua_touserdata(l, -1) == NULL) {
            strbuf_append_mem(json, "null", 4);
            break;
        }
    default:
        /* Remaining types (LUA_TFUNCTION, LUA_TUSERDATA, LUA_TTHREAD,
         * and LUA_TLIGHTUSERDATA) cannot be serialised */
        json_encode_exception(l, cfg, -1, "type not supported");
        /* never returns */
    }
}

static int json_encode(lua_State *l)
{
    json_config_t *cfg;
    char *json;
    int len;

    /* Can't use json_verify_arg_count() since we need to ensure
     * there is only 1 argument */
    luaL_argcheck(l, lua_gettop(l) == 1, 1, "expected 1 argument");

    cfg = json_fetch_config(l);
    cfg->current_depth = 0;

    /* Reset the persistent buffer if it exists.
     * Otherwise allocate a new buffer. */
    if (strbuf_allocated(&cfg->encode_buf))
        strbuf_reset(&cfg->encode_buf);
    else
        strbuf_init(&cfg->encode_buf, 0);

    json_append_data(l, cfg, &cfg->encode_buf);
    json = strbuf_string(&cfg->encode_buf, &len);

    lua_pushlstring(l, json, len);

    if (!cfg->encode_keep_buffer)
        strbuf_free(&cfg->encode_buf);

    return 1;
}

/* ===== DECODING ===== */

static void json_process_value(lua_State *l, json_parse_t *json,
                               json_token_t *token);

static int hexdigit2int(char hex)
{
    if ('0' <= hex  && hex <= '9')
        return hex - '0';

    /* Force lowercase */
    hex |= 0x20;
    if ('a' <= hex && hex <= 'f')
        return 10 + hex - 'a';

    return -1;
}

static int decode_hex4(const char *hex)
{
    int digit[4];
    int i;

    /* Convert ASCII hex digit to numeric digit
     * Note: this returns an error for invalid hex digits, including
     *       NULL */
    for (i = 0; i < 4; i++) {
        digit[i] = hexdigit2int(hex[i]);
        if (digit[i] < 0) {
            return -1;
        }
    }

    return (digit[0] << 12) +
           (digit[1] << 8) +
           (digit[2] << 4) +
            digit[3];
}

/* Converts a Unicode codepoint to UTF-8.
 * Returns UTF-8 string length, and up to 4 bytes in *utf8 */
static int codepoint_to_utf8(char *utf8, int codepoint)
{
    /* 0xxxxxxx */
    if (codepoint <= 0x7F) {
        utf8[0] = codepoint;
        return 1;
    }

    /* 110xxxxx 10xxxxxx */
    if (codepoint <= 0x7FF) {
        utf8[0] = (codepoint >> 6) | 0xC0;
        utf8[1] = (codepoint & 0x3F) | 0x80;
        return 2;
    }

    /* 1110xxxx 10xxxxxx 10xxxxxx */
    if (codepoint <= 0xFFFF) {
        utf8[0] = (codepoint >> 12) | 0xE0;
        utf8[1] = ((codepoint >> 6) & 0x3F) | 0x80;
        utf8[2] = (codepoint & 0x3F) | 0x80;
        return 3;
    }

    /* 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx */
    if (codepoint <= 0x1FFFFF) {
        utf8[0] = (codepoint >> 18) | 0xF0;
        utf8[1] = ((codepoint >> 12) & 0x3F) | 0x80;
        utf8[2] = ((codepoint >> 6) & 0x3F) | 0x80;
        utf8[3] = (codepoint & 0x3F) | 0x80;
        return 4;
    }

    return 0;
}


/* Called when index pointing to beginning of UTF-16 code escape: \uXXXX
 * \u is guaranteed to exist, but the remaining hex characters may be
 * missing.
 * Translate to UTF-8 and append to temporary token string.
 * Must advance index to the next character to be processed.
 * Returns: 0   success
 *          -1  error
 */
static int json_append_unicode_escape(json_parse_t *json)
{
    char utf8[4];       /* Surrogate pairs require 4 UTF-8 bytes */
    int codepoint;
    int surrogate_low;
    int len;
    int escape_len = 6;

    /* Fetch UTF-16 code unit */
    codepoint = decode_hex4(&json->data[json->index + 2]);
    if (codepoint < 0)
        return -1;

    /* UTF-16 surrogate pairs take the following 2 byte form:
     *      11011 x yyyyyyyyyy
     * When x = 0: y is the high 10 bits of the codepoint
     *      x = 1: y is the low 10 bits of the codepoint
     *
     * Check for a surrogate pair (high or low) */
    if ((codepoint & 0xF800) == 0xD800) {
        /* Error if the 1st surrogate is not high */
        if (codepoint & 0x400)
            return -1;

        /* Ensure the next code is a unicode escape */
        if (json->data[json->index + escape_len] != '\\' ||
            json->data[json->index + escape_len + 1] != 'u') {
            return -1;
        }

        /* Fetch the next codepoint */
        surrogate_low = decode_hex4(&json->data[json->index + 2 + escape_len]);
        if (surrogate_low < 0)
            return -1;

        /* Error if the 2nd code is not a low surrogate */
        if ((surrogate_low & 0xFC00) != 0xDC00)
            return -1;

        /* Calculate Unicode codepoint */
        codepoint = (codepoint & 0x3FF) << 10;
        surrogate_low &= 0x3FF;
        codepoint = (codepoint | surrogate_low) + 0x10000;
        escape_len = 12;
    }

    /* Convert codepoint to UTF-8 */
    len = codepoint_to_utf8(utf8, codepoint);
    if (!len)
        return -1;

    /* Append bytes and advance parse index */
    strbuf_append_mem_unsafe(json->tmp, utf8, len);
    json->index += escape_len;

    return 0;
}

static void json_set_token_error(json_token_t *token, json_parse_t *json,
                                 const char *errtype)
{
    token->type = T_ERROR;
    token->index = json->index;
    token->value.string = errtype;
}

static void json_next_string_token(json_parse_t *json, json_token_t *token)
{
    char *escape2char = json->cfg->escape2char;
    char ch;

    /* Caller must ensure a string is next */
    assert(json->data[json->index] == '"');

    /* Skip " */
    json->index++;

    /* json->tmp is the temporary strbuf used to accumulate the
     * decoded string value. */
    strbuf_reset(json->tmp);
    while ((ch = json->data[json->index]) != '"') {
        if (!ch) {
            /* Premature end of the string */
            json_set_token_error(token, json, "unexpected end of string");
            return;
        }

        /* Handle escapes */
        if (ch == '\\') {
            /* Fetch escape character */
            ch = json->data[json->index + 1];

            /* Translate escape code and append to tmp string */
            ch = escape2char[(unsigned char)ch];
            if (ch == 'u') {
                if (json_append_unicode_escape(json) == 0)
                    continue;

                json_set_token_error(token, json,
                                     "invalid unicode escape code");
                return;
            }
            if (!ch) {
                json_set_token_error(token, json, "invalid escape code");
                return;
            }

            /* Skip '\' */
            json->index++;
        }
        /* Append normal character or translated single character
         * Unicode escapes are handled above */
        strbuf_append_char_unsafe(json->tmp, ch);
        json->index++;
    }
    json->index++;  /* Eat final quote (") */

    strbuf_ensure_null(json->tmp);

    token->type = T_STRING;
    token->value.string = strbuf_string(json->tmp, &token->string_len);
}

/* JSON numbers should take the following form:
 *      -?(0|[1-9]|[1-9][0-9]+)(.[0-9]+)?([eE][-+]?[0-9]+)?
 *
 * json_next_number_token() uses strtod() which allows other forms:
 * - numbers starting with '+'
 * - NaN, -NaN, infinity, -infinity
 * - hexidecimal numbers
 * - numbers with leading zeros
 *
 * json_is_invalid_number() detects "numbers" which may pass strtod()'s
 * error checking, but should not be allowed with strict JSON.
 *
 * json_is_invalid_number() may pass numbers which cause strtod()
 * to generate an error.
 */
static int json_is_invalid_number(json_parse_t *json)
{
    int i = json->index;

    /* Reject numbers starting with + */
    if (json->data[i] == '+')
        return 1;

    /* Skip minus sign if it exists */
    if (json->data[i] == '-')
        i++;

    /* Reject numbers starting with 0x, or leading zeros */
    if (json->data[i] == '0') {
        int ch2 = json->data[i + 1];

        if ((ch2 | 0x20) == 'x' ||          /* Hex */
            ('0' <= ch2 && ch2 <= '9'))     /* Leading zero */
            return 1;

        return 0;
    } else if (json->data[i] <= '9') {
        return 0;                           /* Ordinary number */
    }


    /* Reject inf/nan */
    if (!strncasecmp(&json->data[i], "inf", 3))
        return 1;
    if (!strncasecmp(&json->data[i], "nan", 3))
        return 1;

    /* Pass all other numbers which may still be invalid, but
     * strtod() will catch them. */
    return 0;
}

static void json_next_number_token(json_parse_t *json, json_token_t *token)
{
    const char *startptr;
    char *endptr;

    token->type = T_NUMBER;
    startptr = &json->data[json->index];
    token->value.number = strtod(&json->data[json->index], &endptr);
    if (startptr == endptr)
        json_set_token_error(token, json, "invalid number");
    else
        json->index += endptr - startptr;   /* Skip the processed number */

    return;
}

/* Fills in the token struct.
 * T_STRING will return a pointer to the json_parse_t temporary string
 * T_ERROR will leave the json->index pointer at the error.
 */
static void json_next_token(json_parse_t *json, json_token_t *token)
{
    json_token_type_t *ch2token = json->cfg->ch2token;
    int ch;

    /* Eat whitespace. FIXME: UGLY */
    token->type = ch2token[(unsigned char)json->data[json->index]];
    while (token->type == T_WHITESPACE)
        token->type = ch2token[(unsigned char)json->data[++json->index]];

    token->index = json->index;

    /* Don't advance the pointer for an error or the end */
    if (token->type == T_ERROR) {
        json_set_token_error(token, json, "invalid token");
        return;
    }

    if (token->type == T_END) {
        return;
    }

    /* Found a known single character token, advance index and return */
    if (token->type != T_UNKNOWN) {
        json->index++;
        return;
    }

    /* Process characters which triggered T_UNKNOWN */
    ch = json->data[json->index];

    /* Must use strncmp() to match the front of the JSON string.
     * JSON identifier must be lowercase.
     * When strict_numbers if disabled, either case is allowed for
     * Infinity/NaN (since we are no longer following the spec..) */
    if (ch == '"') {
        json_next_string_token(json, token);
        return;
    } else if (ch == '-' || ('0' <= ch && ch <= '9')) {
        if (json->cfg->decode_refuse_badnum && json_is_invalid_number(json)) {
            json_set_token_error(token, json, "invalid number");
            return;
        }
        json_next_number_token(json, token);
        return;
    } else if (!strncmp(&json->data[json->index], "true", 4)) {
        token->type = T_BOOLEAN;
        token->value.boolean = 1;
        json->index += 4;
        return;
    } else if (!strncmp(&json->data[json->index], "false", 5)) {
        token->type = T_BOOLEAN;
        token->value.boolean = 0;
        json->index += 5;
        return;
    } else if (!strncmp(&json->data[json->index], "null", 4)) {
        token->type = T_NULL;
        json->index += 4;
        return;
    } else if (!json->cfg->decode_refuse_badnum &&
               json_is_invalid_number(json)) {
        /* When refuse_badnum is disabled, only attempt to process
         * numbers we know are invalid JSON (Inf, NaN, hex)
         * This is required to generate an appropriate token error,
         * otherwise all bad tokens will register as "invalid number"
         */
        json_next_number_token(json, token);
        return;
    }

    /* Token starts with t/f/n but isn't recognised above. */
    json_set_token_error(token, json, "invalid token");
}

/* This function does not return.
 * DO NOT CALL WITH DYNAMIC MEMORY ALLOCATED.
 * The only supported exception is the temporary parser string
 * json->tmp struct.
 * json and token should exist on the stack somewhere.
 * luaL_error() will long_jmp and release the stack */
static void json_throw_parse_error(lua_State *l, json_parse_t *json,
                                   const char *exp, json_token_t *token)
{
    const char *found;

    strbuf_free(json->tmp);

    if (token->type == T_ERROR)
        found = token->value.string;
    else
        found = json_token_type_name[token->type];

    /* Note: token->index is 0 based, display starting from 1 */
    luaL_error(l, "Expected %s but found %s at character %d",
               exp, found, token->index + 1);
}

static void json_decode_checkstack(lua_State *l, json_parse_t *json, int n)
{
    if (lua_checkstack(l, n))
        return;

    strbuf_free(json->tmp);
    luaL_error(l, "Too many nested data structures");
}

static void json_parse_object_context(lua_State *l, json_parse_t *json)
{
    json_token_t token;

    /* 3 slots required:
     * .., table, key, value */
    json_decode_checkstack(l, json, 3);

    lua_newtable(l);

    json_next_token(json, &token);

    /* Handle empty objects */
    if (token.type == T_OBJ_END) {
        return;
    }

    while (1) {
        if (token.type != T_STRING)
            json_throw_parse_error(l, json, "object key string", &token);

        /* Push key */
        lua_pushlstring(l, token.value.string, token.string_len);

        json_next_token(json, &token);
        if (token.type != T_COLON)
            json_throw_parse_error(l, json, "colon", &token);

        /* Fetch value */
        json_next_token(json, &token);
        json_process_value(l, json, &token);

        /* Set key = value */
        lua_rawset(l, -3);

        json_next_token(json, &token);

        if (token.type == T_OBJ_END)
            return;

        if (token.type != T_COMMA)
            json_throw_parse_error(l, json, "comma or object end", &token);

        json_next_token(json, &token);
    }
}

/* Handle the array context */
static void json_parse_array_context(lua_State *l, json_parse_t *json)
{
    json_token_t token;
    int i;

    /* 2 slots required:
     * .., table, value */
    json_decode_checkstack(l, json, 2);

    lua_newtable(l);

    json_next_token(json, &token);

    /* Handle empty arrays */
    if (token.type == T_ARR_END)
        return;

    for (i = 1; ; i++) {
        json_process_value(l, json, &token);
        lua_rawseti(l, -2, i);            /* arr[i] = value */

        json_next_token(json, &token);

        if (token.type == T_ARR_END)
            return;

        if (token.type != T_COMMA)
            json_throw_parse_error(l, json, "comma or array end", &token);

        json_next_token(json, &token);
    }
}

/* Handle the "value" context */
static void json_process_value(lua_State *l, json_parse_t *json,
                               json_token_t *token)
{
    switch (token->type) {
    case T_STRING:
        lua_pushlstring(l, token->value.string, token->string_len);
        break;;
    case T_NUMBER:
        lua_pushnumber(l, token->value.number);
        break;;
    case T_BOOLEAN:
        lua_pushboolean(l, token->value.boolean);
        break;;
    case T_OBJ_BEGIN:
        json_parse_object_context(l, json);
        break;;
    case T_ARR_BEGIN:
        json_parse_array_context(l, json);
        break;;
    case T_NULL:
        /* In Lua, setting "t[k] = nil" will delete k from the table.
         * Hence a NULL pointer lightuserdata object is used instead */
        lua_pushlightuserdata(l, NULL);
        break;;
    default:
        json_throw_parse_error(l, json, "value", token);
    }
}

/* json_text must be null terminated string */
static void lua_json_decode(lua_State *l, const char *json_text, int json_len)
{
    json_parse_t json;
    json_token_t token;

    json.cfg = json_fetch_config(l);
    json.data = json_text;
    json.index = 0;

    /* Ensure the temporary buffer can hold the entire string.
     * This means we no longer need to do length checks since the decoded
     * string must be smaller than the entire json string */
    json.tmp = strbuf_new(json_len);

    json_next_token(&json, &token);
    json_process_value(l, &json, &token);

    /* Ensure there is no more input left */
    json_next_token(&json, &token);

    if (token.type != T_END)
        json_throw_parse_error(l, &json, "the end", &token);

    strbuf_free(json.tmp);
}

static int json_decode(lua_State *l)
{
    const char *json;
    size_t len;

    json_verify_arg_count(l, 1);

    json = luaL_checklstring(l, 1, &len);

    /* Detect Unicode other than UTF-8 (see RFC 4627, Sec 3)
     *
     * CJSON can support any simple data type, hence only the first
     * character is guaranteed to be ASCII (at worst: '"'). This is
     * still enough to detect whether the wrong encoding is in use. */
    if (len >= 2 && (!json[0] || !json[1]))
        luaL_error(l, "JSON parser does not support UTF-16 or UTF-32");

    lua_json_decode(l, json, len);

    return 1;
}

/* ===== INITIALISATION ===== */

int luaopen_cjson(lua_State *l)
{
    luaL_Reg reg[] = {
        { "encode", json_encode },
        { "decode", json_decode },
        { "encode_sparse_array", json_cfg_encode_sparse_array },
        { "encode_max_depth", json_cfg_encode_max_depth },
        { "encode_keep_buffer", json_cfg_encode_keep_buffer },
        { "refuse_invalid_numbers", json_cfg_refuse_invalid_numbers },
        { NULL, NULL }
    };

    /* Use json_fetch_config as a pointer.
     * It's faster than using a config string, and more unique */
    lua_pushlightuserdata(l, &json_config_key);
    json_create_config(l);
    lua_settable(l, LUA_REGISTRYINDEX);

    luaL_register(l, "cjson", reg);

    /* Set cjson.null */
    lua_pushlightuserdata(l, NULL);
    lua_setfield(l, -2, "null");

    /* Set cjson.version */
    lua_pushliteral(l, VERSION);
    lua_setfield(l, -2, "version");

    /* Return cjson table */
    return 1;
}

/* vi:ai et sw=4 ts=4:
 */

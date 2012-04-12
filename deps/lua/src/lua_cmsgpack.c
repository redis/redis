#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include "lua.h"
#include "lauxlib.h"

#define LUACMSGPACK_VERSION     "lua-cmsgpack 0.3.0"
#define LUACMSGPACK_COPYRIGHT   "Copyright (C) 2012, Salvatore Sanfilippo"
#define LUACMSGPACK_DESCRIPTION "MessagePack C implementation for Lua"

#define LUACMSGPACK_MAX_NESTING  16 /* Max tables nesting. */

/* ==============================================================================
 * MessagePack implementation and bindings for Lua 5.1.
 * Copyright(C) 2012 Salvatore Sanfilippo <antirez@gmail.com>
 *
 * http://github.com/antirez/lua-cmsgpack
 *
 * For MessagePack specification check the following web site:
 * http://wiki.msgpack.org/display/MSGPACK/Format+specification
 *
 * See Copyright Notice at the end of this file.
 *
 * CHANGELOG:
 * 19-Feb-2012 (ver 0.1.0): Initial release.
 * 20-Feb-2012 (ver 0.2.0): Tables encoding improved.
 * 20-Feb-2012 (ver 0.2.1): Minor bug fixing.
 * 20-Feb-2012 (ver 0.3.0): Module renamed lua-cmsgpack (was lua-msgpack).
 * ============================================================================ */

/* --------------------------- Endian conversion --------------------------------
 * We use it only for floats and doubles, all the other conversions are performed
 * in an endian independent fashion. So the only thing we need is a function
 * that swaps a binary string if the arch is little endian (and left it untouched
 * otherwise). */

/* Reverse memory bytes if arch is little endian. Given the conceptual
 * simplicity of the Lua build system we prefer to check for endianess at runtime.
 * The performance difference should be acceptable. */
static void memrevifle(void *ptr, size_t len) {
    unsigned char *p = ptr, *e = p+len-1, aux;
    int test = 1;
    unsigned char *testp = (unsigned char*) &test;

    if (testp[0] == 0) return; /* Big endian, nothign to do. */
    len /= 2;
    while(len--) {
        aux = *p;
        *p = *e;
        *e = aux;
        p++;
        e--;
    }
}

/* ----------------------------- String buffer ----------------------------------
 * This is a simple implementation of string buffers. The only opereation
 * supported is creating empty buffers and appending bytes to it.
 * The string buffer uses 2x preallocation on every realloc for O(N) append
 * behavior.  */

typedef struct mp_buf {
    unsigned char *b;
    size_t len, free;
} mp_buf;

static mp_buf *mp_buf_new(void) {
    mp_buf *buf = malloc(sizeof(*buf));
    
    buf->b = NULL;
    buf->len = buf->free = 0;
    return buf;
}

void mp_buf_append(mp_buf *buf, const unsigned char *s, size_t len) {
    if (buf->free < len) {
        size_t newlen = buf->len+len;

        buf->b = realloc(buf->b,newlen*2);
        buf->free = newlen;
    }
    memcpy(buf->b+buf->len,s,len);
    buf->len += len;
    buf->free -= len;
}

void mp_buf_free(mp_buf *buf) {
    free(buf->b);
    free(buf);
}

/* ------------------------------ String cursor ----------------------------------
 * This simple data structure is used for parsing. Basically you create a cursor
 * using a string pointer and a length, then it is possible to access the
 * current string position with cursor->p, check the remaining length
 * in cursor->left, and finally consume more string using
 * mp_cur_consume(cursor,len), to advance 'p' and subtract 'left'.
 * An additional field cursor->error is set to zero on initialization and can
 * be used to report errors. */

#define MP_CUR_ERROR_NONE   0
#define MP_CUR_ERROR_EOF    1   /* Not enough data to complete the opereation. */
#define MP_CUR_ERROR_BADFMT 2   /* Bad data format */

typedef struct mp_cur {
    const unsigned char *p;
    size_t left;
    int err;
} mp_cur;

static mp_cur *mp_cur_new(const unsigned char *s, size_t len) {
    mp_cur *cursor = malloc(sizeof(*cursor));

    cursor->p = s;
    cursor->left = len;
    cursor->err = MP_CUR_ERROR_NONE;
    return cursor;
}

static void mp_cur_free(mp_cur *cursor) {
    free(cursor);
}

#define mp_cur_consume(_c,_len) do { _c->p += _len; _c->left -= _len; } while(0)

/* When there is not enough room we set an error in the cursor and return, this
 * is very common across the code so we have a macro to make the code look
 * a bit simpler. */
#define mp_cur_need(_c,_len) do { \
    if (_c->left < _len) { \
        _c->err = MP_CUR_ERROR_EOF; \
        return; \
    } \
} while(0)

/* --------------------------- Low level MP encoding -------------------------- */

static void mp_encode_bytes(mp_buf *buf, const unsigned char *s, size_t len) {
    unsigned char hdr[5];
    int hdrlen;

    if (len < 32) {
        hdr[0] = 0xa0 | (len&0xff); /* fix raw */
        hdrlen = 1;
    } else if (len <= 0xffff) {
        hdr[0] = 0xda;
        hdr[1] = (len&0xff00)>>8;
        hdr[2] = len&0xff;
        hdrlen = 3;
    } else {
        hdr[0] = 0xdb;
        hdr[1] = (len&0xff000000)>>24;
        hdr[2] = (len&0xff0000)>>16;
        hdr[3] = (len&0xff00)>>8;
        hdr[4] = len&0xff;
        hdrlen = 5;
    }
    mp_buf_append(buf,hdr,hdrlen);
    mp_buf_append(buf,s,len);
}

/* we assume IEEE 754 internal format for single and double precision floats. */
static void mp_encode_double(mp_buf *buf, double d) {
    unsigned char b[9];
    float f = d;

    assert(sizeof(f) == 4 && sizeof(d) == 8);
    if (d == (double)f) {
        b[0] = 0xca;    /* float IEEE 754 */
        memcpy(b+1,&f,4);
        memrevifle(b+1,4);
        mp_buf_append(buf,b,5);
    } else if (sizeof(d) == 8) {
        b[0] = 0xcb;    /* double IEEE 754 */
        memcpy(b+1,&d,8);
        memrevifle(b+1,8);
        mp_buf_append(buf,b,9);
    }
}

static void mp_encode_int(mp_buf *buf, int64_t n) {
    unsigned char b[9];
    int enclen;

    if (n >= 0) {
        if (n <= 127) {
            b[0] = n & 0x7f;    /* positive fixnum */
            enclen = 1;
        } else if (n <= 0xff) {
            b[0] = 0xcc;        /* uint 8 */
            b[1] = n & 0xff;
            enclen = 2;
        } else if (n <= 0xffff) {
            b[0] = 0xcd;        /* uint 16 */
            b[1] = (n & 0xff00) >> 8;
            b[2] = n & 0xff;
            enclen = 3;
        } else if (n <= 0xffffffffLL) {
            b[0] = 0xce;        /* uint 32 */
            b[1] = (n & 0xff000000) >> 24;
            b[2] = (n & 0xff0000) >> 16;
            b[3] = (n & 0xff00) >> 8;
            b[4] = n & 0xff;
            enclen = 5;
        } else {
            b[0] = 0xcf;        /* uint 64 */
            b[1] = (n & 0xff00000000000000LL) >> 56;
            b[2] = (n & 0xff000000000000LL) >> 48;
            b[3] = (n & 0xff0000000000LL) >> 40;
            b[4] = (n & 0xff00000000LL) >> 32;
            b[5] = (n & 0xff000000) >> 24;
            b[6] = (n & 0xff0000) >> 16;
            b[7] = (n & 0xff00) >> 8;
            b[8] = n & 0xff;
            enclen = 9;
        }
    } else {
        if (n >= -32) {
            b[0] = ((char)n);   /* negative fixnum */
            enclen = 1;
        } else if (n >= -128) {
            b[0] = 0xd0;        /* int 8 */
            b[1] = n & 0xff;
            enclen = 2;
        } else if (n >= -32768) {
            b[0] = 0xd1;        /* int 16 */
            b[1] = (n & 0xff00) >> 8;
            b[2] = n & 0xff;
            enclen = 3;
        } else if (n >= -2147483648LL) {
            b[0] = 0xd2;        /* int 32 */
            b[1] = (n & 0xff000000) >> 24;
            b[2] = (n & 0xff0000) >> 16;
            b[3] = (n & 0xff00) >> 8;
            b[4] = n & 0xff;
            enclen = 5;
        } else {
            b[0] = 0xd3;        /* int 64 */
            b[1] = (n & 0xff00000000000000LL) >> 56;
            b[2] = (n & 0xff000000000000LL) >> 48;
            b[3] = (n & 0xff0000000000LL) >> 40;
            b[4] = (n & 0xff00000000LL) >> 32;
            b[5] = (n & 0xff000000) >> 24;
            b[6] = (n & 0xff0000) >> 16;
            b[7] = (n & 0xff00) >> 8;
            b[8] = n & 0xff;
            enclen = 9;
        }
    }
    mp_buf_append(buf,b,enclen);
}

static void mp_encode_array(mp_buf *buf, int64_t n) {
    unsigned char b[5];
    int enclen;

    if (n <= 15) {
        b[0] = 0x90 | (n & 0xf);    /* fix array */
        enclen = 1;
    } else if (n <= 65535) {
        b[0] = 0xdc;                /* array 16 */
        b[1] = (n & 0xff00) >> 8;
        b[2] = n & 0xff;
        enclen = 3;
    } else {
        b[0] = 0xdd;                /* array 32 */
        b[1] = (n & 0xff000000) >> 24;
        b[2] = (n & 0xff0000) >> 16;
        b[3] = (n & 0xff00) >> 8;
        b[4] = n & 0xff;
        enclen = 5;
    }
    mp_buf_append(buf,b,enclen);
}

static void mp_encode_map(mp_buf *buf, int64_t n) {
    unsigned char b[5];
    int enclen;

    if (n <= 15) {
        b[0] = 0x80 | (n & 0xf);    /* fix map */
        enclen = 1;
    } else if (n <= 65535) {
        b[0] = 0xde;                /* map 16 */
        b[1] = (n & 0xff00) >> 8;
        b[2] = n & 0xff;
        enclen = 3;
    } else {
        b[0] = 0xdf;                /* map 32 */
        b[1] = (n & 0xff000000) >> 24;
        b[2] = (n & 0xff0000) >> 16;
        b[3] = (n & 0xff00) >> 8;
        b[4] = n & 0xff;
        enclen = 5;
    }
    mp_buf_append(buf,b,enclen);
}

/* ----------------------------- Lua types encoding --------------------------- */

static void mp_encode_lua_string(lua_State *L, mp_buf *buf) {
    size_t len;
    const char *s;

    s = lua_tolstring(L,-1,&len);
    mp_encode_bytes(buf,(const unsigned char*)s,len);
}

static void mp_encode_lua_bool(lua_State *L, mp_buf *buf) {
    unsigned char b = lua_toboolean(L,-1) ? 0xc3 : 0xc2;
    mp_buf_append(buf,&b,1);
}

static void mp_encode_lua_number(lua_State *L, mp_buf *buf) {
    lua_Number n = lua_tonumber(L,-1);

    if (floor(n) != n) {
        mp_encode_double(buf,(double)n);
    } else {
        mp_encode_int(buf,(int64_t)n);
    }
}

static void mp_encode_lua_type(lua_State *L, mp_buf *buf, int level);

/* Convert a lua table into a message pack list. */
static void mp_encode_lua_table_as_array(lua_State *L, mp_buf *buf, int level) {
    size_t len = lua_objlen(L,-1), j;

    mp_encode_array(buf,len);
    for (j = 1; j <= len; j++) {
        lua_pushnumber(L,j);
        lua_gettable(L,-2);
        mp_encode_lua_type(L,buf,level+1);
    }
}

/* Convert a lua table into a message pack key-value map. */
static void mp_encode_lua_table_as_map(lua_State *L, mp_buf *buf, int level) {
    size_t len = 0;

    /* First step: count keys into table. No other way to do it with the
     * Lua API, we need to iterate a first time. Note that an alternative
     * would be to do a single run, and then hack the buffer to insert the
     * map opcodes for message pack. Too hachish for this lib. */
    lua_pushnil(L);
    while(lua_next(L,-2)) {
        lua_pop(L,1); /* remove value, keep key for next iteration. */
        len++;
    }

    /* Step two: actually encoding of the map. */
    mp_encode_map(buf,len);
    lua_pushnil(L);
    while(lua_next(L,-2)) {
        /* Stack: ... key value */
        lua_pushvalue(L,-2); /* Stack: ... key value key */
        mp_encode_lua_type(L,buf,level+1); /* encode key */
        mp_encode_lua_type(L,buf,level+1); /* encode val */
    }
}

/* Returns true if the Lua table on top of the stack is exclusively composed
 * of keys from numerical keys from 1 up to N, with N being the total number
 * of elements, without any hole in the middle. */
static int table_is_an_array(lua_State *L) {
    long count = 0, max = 0, idx = 0;
    lua_Number n;

    lua_pushnil(L);
    while(lua_next(L,-2)) {
        /* Stack: ... key value */
        lua_pop(L,1); /* Stack: ... key */
        if (!lua_isnumber(L,-1)) goto not_array;
        n = lua_tonumber(L,-1);
        idx = n;
        if (idx != n || idx < 1) goto not_array;
        count++;
        max = idx;
    }
    /* We have the total number of elements in "count". Also we have
     * the max index encountered in "idx". We can't reach this code
     * if there are indexes <= 0. If you also note that there can not be
     * repeated keys into a table, you have that if idx==count you are sure
     * that there are all the keys form 1 to count (both included). */
    return idx == count;

not_array:
    lua_pop(L,1);
    return 0;
}

/* If the length operator returns non-zero, that is, there is at least
 * an object at key '1', we serialize to message pack list. Otherwise
 * we use a map. */
static void mp_encode_lua_table(lua_State *L, mp_buf *buf, int level) {
    if (table_is_an_array(L))
        mp_encode_lua_table_as_array(L,buf,level);
    else
        mp_encode_lua_table_as_map(L,buf,level);
}

static void mp_encode_lua_null(lua_State *L, mp_buf *buf) {
    unsigned char b[1];

    b[0] = 0xc0;
    mp_buf_append(buf,b,1);
}

static void mp_encode_lua_type(lua_State *L, mp_buf *buf, int level) {
    int t = lua_type(L,-1);

    /* Limit the encoding of nested tables to a specfiied maximum depth, so that
     * we survive when called against circular references in tables. */
    if (t == LUA_TTABLE && level == LUACMSGPACK_MAX_NESTING) t = LUA_TNIL;
    switch(t) {
    case LUA_TSTRING: mp_encode_lua_string(L,buf); break;
    case LUA_TBOOLEAN: mp_encode_lua_bool(L,buf); break;
    case LUA_TNUMBER: mp_encode_lua_number(L,buf); break;
    case LUA_TTABLE: mp_encode_lua_table(L,buf,level); break;
    default: mp_encode_lua_null(L,buf); break;
    }
    lua_pop(L,1);
}

static int mp_pack(lua_State *L) {
    mp_buf *buf = mp_buf_new();

    mp_encode_lua_type(L,buf,0);
    lua_pushlstring(L,(char*)buf->b,buf->len);
    mp_buf_free(buf);
    return 1;
}

/* --------------------------------- Decoding --------------------------------- */

void mp_decode_to_lua_type(lua_State *L, mp_cur *c);

void mp_decode_to_lua_array(lua_State *L, mp_cur *c, size_t len) {
    int index = 1;

    lua_newtable(L);
    while(len--) {
        lua_pushnumber(L,index++);
        mp_decode_to_lua_type(L,c);
        if (c->err) return;
        lua_settable(L,-3);
    }
}

void mp_decode_to_lua_hash(lua_State *L, mp_cur *c, size_t len) {
    lua_newtable(L);
    while(len--) {
        mp_decode_to_lua_type(L,c); /* key */
        if (c->err) return;
        mp_decode_to_lua_type(L,c); /* value */
        if (c->err) return;
        lua_settable(L,-3);
    }
}

/* Decode a Message Pack raw object pointed by the string cursor 'c' to
 * a Lua type, that is left as the only result on the stack. */
void mp_decode_to_lua_type(lua_State *L, mp_cur *c) {
    mp_cur_need(c,1);
    switch(c->p[0]) {
    case 0xcc:  /* uint 8 */
        mp_cur_need(c,2);
        lua_pushnumber(L,c->p[1]);
        mp_cur_consume(c,2);
        break;
    case 0xd0:  /* int 8 */
        mp_cur_need(c,2);
        lua_pushnumber(L,(char)c->p[1]);
        mp_cur_consume(c,2);
        break;
    case 0xcd:  /* uint 16 */
        mp_cur_need(c,3);
        lua_pushnumber(L,
            (c->p[1] << 8) |
             c->p[2]);
        mp_cur_consume(c,3);
        break;
    case 0xd1:  /* int 16 */
        mp_cur_need(c,3);
        lua_pushnumber(L,(int16_t)
            (c->p[1] << 8) |
             c->p[2]);
        mp_cur_consume(c,3);
        break;
    case 0xce:  /* uint 32 */
        mp_cur_need(c,5);
        lua_pushnumber(L,
            ((uint32_t)c->p[1] << 24) |
            ((uint32_t)c->p[2] << 16) |
            ((uint32_t)c->p[3] << 8) |
             (uint32_t)c->p[4]);
        mp_cur_consume(c,5);
        break;
    case 0xd2:  /* int 32 */
        mp_cur_need(c,5);
        lua_pushnumber(L,
            ((int32_t)c->p[1] << 24) |
            ((int32_t)c->p[2] << 16) |
            ((int32_t)c->p[3] << 8) |
             (int32_t)c->p[4]);
        mp_cur_consume(c,5);
        break;
    case 0xcf:  /* uint 64 */
        mp_cur_need(c,9);
        lua_pushnumber(L,
            ((uint64_t)c->p[1] << 56) |
            ((uint64_t)c->p[2] << 48) |
            ((uint64_t)c->p[3] << 40) |
            ((uint64_t)c->p[4] << 32) |
            ((uint64_t)c->p[5] << 24) |
            ((uint64_t)c->p[6] << 16) |
            ((uint64_t)c->p[7] << 8) |
             (uint64_t)c->p[8]);
        mp_cur_consume(c,9);
        break;
    case 0xd3:  /* int 64 */
        mp_cur_need(c,9);
        lua_pushnumber(L,
            ((int64_t)c->p[1] << 56) |
            ((int64_t)c->p[2] << 48) |
            ((int64_t)c->p[3] << 40) |
            ((int64_t)c->p[4] << 32) |
            ((int64_t)c->p[5] << 24) |
            ((int64_t)c->p[6] << 16) |
            ((int64_t)c->p[7] << 8) |
             (int64_t)c->p[8]);
        mp_cur_consume(c,9);
        break;
    case 0xc0:  /* nil */
        lua_pushnil(L);
        mp_cur_consume(c,1);
        break;
    case 0xc3:  /* true */
        lua_pushboolean(L,1);
        mp_cur_consume(c,1);
        break;
    case 0xc2:  /* false */
        lua_pushboolean(L,0);
        mp_cur_consume(c,1);
        break;
    case 0xca:  /* float */
        mp_cur_need(c,5);
        assert(sizeof(float) == 4);
        {
            float f;
            memcpy(&f,c->p+1,4);
            memrevifle(&f,4);
            lua_pushnumber(L,f);
            mp_cur_consume(c,5);
        }
        break;
    case 0xcb:  /* double */
        mp_cur_need(c,9);
        assert(sizeof(double) == 8);
        {
            double d;
            memcpy(&d,c->p+1,8);
            memrevifle(&d,8);
            lua_pushnumber(L,d);
            mp_cur_consume(c,9);
        }
        break;
    case 0xda:  /* raw 16 */
        mp_cur_need(c,3);
        {
            size_t l = (c->p[1] << 8) | c->p[2];
            mp_cur_need(c,3+l);
            lua_pushlstring(L,(char*)c->p+3,l);
            mp_cur_consume(c,3+l);
        }
        break;
    case 0xdb:  /* raw 32 */
        mp_cur_need(c,5);
        {
            size_t l = (c->p[1] << 24) |
                       (c->p[2] << 16) |
                       (c->p[3] << 8) |
                       c->p[4];
            mp_cur_need(c,5+l);
            lua_pushlstring(L,(char*)c->p+5,l);
            mp_cur_consume(c,5+l);
        }
        break;
    case 0xdc:  /* array 16 */
        mp_cur_need(c,3);
        {
            size_t l = (c->p[1] << 8) | c->p[2];
            mp_cur_consume(c,3);
            mp_decode_to_lua_array(L,c,l);
        }
        break;
    case 0xdd:  /* array 32 */
        mp_cur_need(c,5);
        {
            size_t l = (c->p[1] << 24) |
                       (c->p[2] << 16) |
                       (c->p[3] << 8) |
                       c->p[4];
            mp_cur_consume(c,5);
            mp_decode_to_lua_array(L,c,l);
        }
        break;
    case 0xde:  /* map 16 */
        mp_cur_need(c,3);
        {
            size_t l = (c->p[1] << 8) | c->p[2];
            mp_cur_consume(c,3);
            mp_decode_to_lua_hash(L,c,l);
        }
        break;
    case 0xdf:  /* map 32 */
        mp_cur_need(c,5);
        {
            size_t l = (c->p[1] << 24) |
                       (c->p[2] << 16) |
                       (c->p[3] << 8) |
                       c->p[4];
            mp_cur_consume(c,5);
            mp_decode_to_lua_hash(L,c,l);
        }
        break;
    default:    /* types that can't be idenitified by first byte value. */
        if ((c->p[0] & 0x80) == 0) {   /* positive fixnum */
            lua_pushnumber(L,c->p[0]);
            mp_cur_consume(c,1);
        } else if ((c->p[0] & 0xe0) == 0xe0) {  /* negative fixnum */
            lua_pushnumber(L,(signed char)c->p[0]);
            mp_cur_consume(c,1);
        } else if ((c->p[0] & 0xe0) == 0xa0) {  /* fix raw */
            size_t l = c->p[0] & 0x1f;
            mp_cur_need(c,1+l);
            lua_pushlstring(L,(char*)c->p+1,l);
            mp_cur_consume(c,1+l);
        } else if ((c->p[0] & 0xf0) == 0x90) {  /* fix map */
            size_t l = c->p[0] & 0xf;
            mp_cur_consume(c,1);
            mp_decode_to_lua_array(L,c,l);
        } else if ((c->p[0] & 0xf0) == 0x80) {  /* fix map */
            size_t l = c->p[0] & 0xf;
            mp_cur_consume(c,1);
            mp_decode_to_lua_hash(L,c,l);
        } else {
            c->err = MP_CUR_ERROR_BADFMT;
        }
    }
}

static int mp_unpack(lua_State *L) {
    size_t len;
    const unsigned char *s;
    mp_cur *c;

    if (!lua_isstring(L,-1)) {
        lua_pushstring(L,"MessagePack decoding needs a string as input.");
        lua_error(L);
    }

    s = (const unsigned char*) lua_tolstring(L,-1,&len);
    c = mp_cur_new(s,len);
    mp_decode_to_lua_type(L,c);
    
    if (c->err == MP_CUR_ERROR_EOF) {
        mp_cur_free(c);
        lua_pushstring(L,"Missing bytes in input.");
        lua_error(L);
    } else if (c->err == MP_CUR_ERROR_BADFMT) {
        mp_cur_free(c);
        lua_pushstring(L,"Bad data format in input.");
        lua_error(L);
    } else if (c->left != 0) {
        mp_cur_free(c);
        lua_pushstring(L,"Extra bytes in input.");
        lua_error(L);
    }
    mp_cur_free(c);
    return 1;
}

/* ---------------------------------------------------------------------------- */

static const struct luaL_reg thislib[] = {
    {"pack", mp_pack},
    {"unpack", mp_unpack},
    {NULL, NULL}
};

LUALIB_API int luaopen_cmsgpack (lua_State *L) {
    luaL_register(L, "cmsgpack", thislib);

    lua_pushliteral(L, LUACMSGPACK_VERSION);
    lua_setfield(L, -2, "_VERSION");
    lua_pushliteral(L, LUACMSGPACK_COPYRIGHT);
    lua_setfield(L, -2, "_COPYRIGHT");
    lua_pushliteral(L, LUACMSGPACK_DESCRIPTION);
    lua_setfield(L, -2, "_DESCRIPTION"); 
    return 1;
}

/******************************************************************************
* Copyright (C) 2012 Salvatore Sanfilippo.  All rights reserved.
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
******************************************************************************/

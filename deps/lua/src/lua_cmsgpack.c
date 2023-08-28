#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include "lua.h"
#include "lauxlib.h"

#define LUACMSGPACK_NAME        "cmsgpack"
#define LUACMSGPACK_SAFE_NAME   "cmsgpack_safe"
#define LUACMSGPACK_VERSION     "lua-cmsgpack 0.4.0"
#define LUACMSGPACK_COPYRIGHT   "Copyright (C) 2012, Salvatore Sanfilippo"
#define LUACMSGPACK_DESCRIPTION "MessagePack C implementation for Lua"

/* Allows a preprocessor directive to override MAX_NESTING */
#ifndef LUACMSGPACK_MAX_NESTING
    #define LUACMSGPACK_MAX_NESTING  16 /* Max tables nesting. */
#endif

/* Check if float or double can be an integer without loss of precision */
#define IS_INT_TYPE_EQUIVALENT(x, T) (!isinf(x) && (T)(x) == (x))

#define IS_INT64_EQUIVALENT(x) IS_INT_TYPE_EQUIVALENT(x, int64_t)
#define IS_INT_EQUIVALENT(x) IS_INT_TYPE_EQUIVALENT(x, int)

/* If size of pointer is equal to a 4 byte integer, we're on 32 bits. */
#if UINTPTR_MAX == UINT_MAX
    #define BITS_32 1
#else
    #define BITS_32 0
#endif

#if BITS_32
    #define lua_pushunsigned(L, n) lua_pushnumber(L, n)
#else
    #define lua_pushunsigned(L, n) lua_pushinteger(L, n)
#endif

/* =============================================================================
 * MessagePack implementation and bindings for Lua 5.1/5.2.
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
 * 04-Apr-2014 (ver 0.3.1): Lua 5.2 support and minor bug fix.
 * 07-Apr-2014 (ver 0.4.0): Multiple pack/unpack, lua allocator, efficiency.
 * ========================================================================== */

/* -------------------------- Endian conversion --------------------------------
 * We use it only for floats and doubles, all the other conversions performed
 * in an endian independent fashion. So the only thing we need is a function
 * that swaps a binary string if arch is little endian (and left it untouched
 * otherwise). */

/* Reverse memory bytes if arch is little endian. Given the conceptual
 * simplicity of the Lua build system we prefer check for endianess at runtime.
 * The performance difference should be acceptable. */
void memrevifle(void *ptr, size_t len) {
    unsigned char   *p = (unsigned char *)ptr,
                    *e = (unsigned char *)p+len-1,
                    aux;
    int test = 1;
    unsigned char *testp = (unsigned char*) &test;

    if (testp[0] == 0) return; /* Big endian, nothing to do. */
    len /= 2;
    while(len--) {
        aux = *p;
        *p = *e;
        *e = aux;
        p++;
        e--;
    }
}

/* ---------------------------- String buffer ----------------------------------
 * This is a simple implementation of string buffers. The only operation
 * supported is creating empty buffers and appending bytes to it.
 * The string buffer uses 2x preallocation on every realloc for O(N) append
 * behavior.  */

typedef struct mp_buf {
    unsigned char *b;
    size_t len, free;
} mp_buf;

void *mp_realloc(lua_State *L, void *target, size_t osize,size_t nsize) {
    void *(*local_realloc) (void *, void *, size_t osize, size_t nsize) = NULL;
    void *ud;

    local_realloc = lua_getallocf(L, &ud);

    return local_realloc(ud, target, osize, nsize);
}

mp_buf *mp_buf_new(lua_State *L) {
    mp_buf *buf = NULL;

    /* Old size = 0; new size = sizeof(*buf) */
    buf = (mp_buf*)mp_realloc(L, NULL, 0, sizeof(*buf));

    buf->b = NULL;
    buf->len = buf->free = 0;
    return buf;
}

void mp_buf_append(lua_State *L, mp_buf *buf, const unsigned char *s, size_t len) {
    if (buf->free < len) {
        size_t newsize = buf->len+len;
        if (newsize < buf->len || newsize >= SIZE_MAX/2) abort();
        newsize *= 2;

        buf->b = (unsigned char*)mp_realloc(L, buf->b, buf->len + buf->free, newsize);
        buf->free = newsize - buf->len;
    }
    memcpy(buf->b+buf->len,s,len);
    buf->len += len;
    buf->free -= len;
}

void mp_buf_free(lua_State *L, mp_buf *buf) {
    mp_realloc(L, buf->b, buf->len + buf->free, 0); /* realloc to 0 = free */
    mp_realloc(L, buf, sizeof(*buf), 0);
}

/* ---------------------------- String cursor ----------------------------------
 * This simple data structure is used for parsing. Basically you create a cursor
 * using a string pointer and a length, then it is possible to access the
 * current string position with cursor->p, check the remaining length
 * in cursor->left, and finally consume more string using
 * mp_cur_consume(cursor,len), to advance 'p' and subtract 'left'.
 * An additional field cursor->error is set to zero on initialization and can
 * be used to report errors. */

#define MP_CUR_ERROR_NONE   0
#define MP_CUR_ERROR_EOF    1   /* Not enough data to complete operation. */
#define MP_CUR_ERROR_BADFMT 2   /* Bad data format */

typedef struct mp_cur {
    const unsigned char *p;
    size_t left;
    int err;
} mp_cur;

void mp_cur_init(mp_cur *cursor, const unsigned char *s, size_t len) {
    cursor->p = s;
    cursor->left = len;
    cursor->err = MP_CUR_ERROR_NONE;
}

#define mp_cur_consume(_c,_len) do { _c->p += _len; _c->left -= _len; } while(0)

/* When there is not enough room we set an error in the cursor and return. This
 * is very common across the code so we have a macro to make the code look
 * a bit simpler. */
#define mp_cur_need(_c,_len) do { \
    if (_c->left < _len) { \
        _c->err = MP_CUR_ERROR_EOF; \
        return; \
    } \
} while(0)

/* ------------------------- Low level MP encoding -------------------------- */

void mp_encode_bytes(lua_State *L, mp_buf *buf, const unsigned char *s, size_t len) {
    unsigned char hdr[5];
    size_t hdrlen;

    if (len < 32) {
        hdr[0] = 0xa0 | (len&0xff); /* fix raw */
        hdrlen = 1;
    } else if (len <= 0xff) {
        hdr[0] = 0xd9;
        hdr[1] = len;
        hdrlen = 2;
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
    mp_buf_append(L,buf,hdr,hdrlen);
    mp_buf_append(L,buf,s,len);
}

/* we assume IEEE 754 internal format for single and double precision floats. */
void mp_encode_double(lua_State *L, mp_buf *buf, double d) {
    unsigned char b[9];
    float f = d;

    assert(sizeof(f) == 4 && sizeof(d) == 8);
    if (d == (double)f) {
        b[0] = 0xca;    /* float IEEE 754 */
        memcpy(b+1,&f,4);
        memrevifle(b+1,4);
        mp_buf_append(L,buf,b,5);
    } else if (sizeof(d) == 8) {
        b[0] = 0xcb;    /* double IEEE 754 */
        memcpy(b+1,&d,8);
        memrevifle(b+1,8);
        mp_buf_append(L,buf,b,9);
    }
}

void mp_encode_int(lua_State *L, mp_buf *buf, int64_t n) {
    unsigned char b[9];
    size_t enclen;

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
            b[0] = ((signed char)n);   /* negative fixnum */
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
    mp_buf_append(L,buf,b,enclen);
}

void mp_encode_array(lua_State *L, mp_buf *buf, uint64_t n) {
    unsigned char b[5];
    size_t enclen;

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
    mp_buf_append(L,buf,b,enclen);
}

void mp_encode_map(lua_State *L, mp_buf *buf, uint64_t n) {
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
    mp_buf_append(L,buf,b,enclen);
}

/* --------------------------- Lua types encoding --------------------------- */

void mp_encode_lua_string(lua_State *L, mp_buf *buf) {
    size_t len;
    const char *s;

    s = lua_tolstring(L,-1,&len);
    mp_encode_bytes(L,buf,(const unsigned char*)s,len);
}

void mp_encode_lua_bool(lua_State *L, mp_buf *buf) {
    unsigned char b = lua_toboolean(L,-1) ? 0xc3 : 0xc2;
    mp_buf_append(L,buf,&b,1);
}

/* Lua 5.3 has a built in 64-bit integer type */
void mp_encode_lua_integer(lua_State *L, mp_buf *buf) {
#if (LUA_VERSION_NUM < 503) && BITS_32
    lua_Number i = lua_tonumber(L,-1);
#else
    lua_Integer i = lua_tointeger(L,-1);
#endif
    mp_encode_int(L, buf, (int64_t)i);
}

/* Lua 5.2 and lower only has 64-bit doubles, so we need to
 * detect if the double may be representable as an int
 * for Lua < 5.3 */
void mp_encode_lua_number(lua_State *L, mp_buf *buf) {
    lua_Number n = lua_tonumber(L,-1);

    if (IS_INT64_EQUIVALENT(n)) {
        mp_encode_lua_integer(L, buf);
    } else {
        mp_encode_double(L,buf,(double)n);
    }
}

void mp_encode_lua_type(lua_State *L, mp_buf *buf, int level);

/* Convert a lua table into a message pack list. */
void mp_encode_lua_table_as_array(lua_State *L, mp_buf *buf, int level) {
#if LUA_VERSION_NUM < 502
    size_t len = lua_objlen(L,-1), j;
#else
    size_t len = lua_rawlen(L,-1), j;
#endif

    mp_encode_array(L,buf,len);
    luaL_checkstack(L, 1, "in function mp_encode_lua_table_as_array");
    for (j = 1; j <= len; j++) {
        lua_pushnumber(L,j);
        lua_gettable(L,-2);
        mp_encode_lua_type(L,buf,level+1);
    }
}

/* Convert a lua table into a message pack key-value map. */
void mp_encode_lua_table_as_map(lua_State *L, mp_buf *buf, int level) {
    size_t len = 0;

    /* First step: count keys into table. No other way to do it with the
     * Lua API, we need to iterate a first time. Note that an alternative
     * would be to do a single run, and then hack the buffer to insert the
     * map opcodes for message pack. Too hackish for this lib. */
    luaL_checkstack(L, 3, "in function mp_encode_lua_table_as_map");
    lua_pushnil(L);
    while(lua_next(L,-2)) {
        lua_pop(L,1); /* remove value, keep key for next iteration. */
        len++;
    }

    /* Step two: actually encoding of the map. */
    mp_encode_map(L,buf,len);
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
int table_is_an_array(lua_State *L) {
    int count = 0, max = 0;
#if LUA_VERSION_NUM < 503
    lua_Number n;
#else
    lua_Integer n;
#endif

    /* Stack top on function entry */
    int stacktop;

    stacktop = lua_gettop(L);

    luaL_checkstack(L, 2, "in function table_is_an_array");
    lua_pushnil(L);
    while(lua_next(L,-2)) {
        /* Stack: ... key value */
        lua_pop(L,1); /* Stack: ... key */
        /* The <= 0 check is valid here because we're comparing indexes. */
#if LUA_VERSION_NUM < 503
        if ((LUA_TNUMBER != lua_type(L,-1)) || (n = lua_tonumber(L, -1)) <= 0 ||
            !IS_INT_EQUIVALENT(n))
#else
        if (!lua_isinteger(L,-1) || (n = lua_tointeger(L, -1)) <= 0)
#endif
        {
            lua_settop(L, stacktop);
            return 0;
        }
        max = (n > max ? n : max);
        count++;
    }
    /* We have the total number of elements in "count". Also we have
     * the max index encountered in "max". We can't reach this code
     * if there are indexes <= 0. If you also note that there can not be
     * repeated keys into a table, you have that if max==count you are sure
     * that there are all the keys form 1 to count (both included). */
    lua_settop(L, stacktop);
    return max == count;
}

/* If the length operator returns non-zero, that is, there is at least
 * an object at key '1', we serialize to message pack list. Otherwise
 * we use a map. */
void mp_encode_lua_table(lua_State *L, mp_buf *buf, int level) {
    if (table_is_an_array(L))
        mp_encode_lua_table_as_array(L,buf,level);
    else
        mp_encode_lua_table_as_map(L,buf,level);
}

void mp_encode_lua_null(lua_State *L, mp_buf *buf) {
    unsigned char b[1];

    b[0] = 0xc0;
    mp_buf_append(L,buf,b,1);
}

void mp_encode_lua_type(lua_State *L, mp_buf *buf, int level) {
    int t = lua_type(L,-1);

    /* Limit the encoding of nested tables to a specified maximum depth, so that
     * we survive when called against circular references in tables. */
    if (t == LUA_TTABLE && level == LUACMSGPACK_MAX_NESTING) t = LUA_TNIL;
    switch(t) {
    case LUA_TSTRING: mp_encode_lua_string(L,buf); break;
    case LUA_TBOOLEAN: mp_encode_lua_bool(L,buf); break;
    case LUA_TNUMBER:
    #if LUA_VERSION_NUM < 503
        mp_encode_lua_number(L,buf); break;
    #else
        if (lua_isinteger(L, -1)) {
            mp_encode_lua_integer(L, buf);
        } else {
            mp_encode_lua_number(L, buf);
        }
        break;
    #endif
    case LUA_TTABLE: mp_encode_lua_table(L,buf,level); break;
    default: mp_encode_lua_null(L,buf); break;
    }
    lua_pop(L,1);
}

/*
 * Packs all arguments as a stream for multiple upacking later.
 * Returns error if no arguments provided.
 */
int mp_pack(lua_State *L) {
    int nargs = lua_gettop(L);
    int i;
    mp_buf *buf;

    if (nargs == 0)
        return luaL_argerror(L, 0, "MessagePack pack needs input.");

    if (!lua_checkstack(L, nargs))
        return luaL_argerror(L, 0, "Too many arguments for MessagePack pack.");

    buf = mp_buf_new(L);
    for(i = 1; i <= nargs; i++) {
        /* Copy argument i to top of stack for _encode processing;
         * the encode function pops it from the stack when complete. */
        luaL_checkstack(L, 1, "in function mp_check");
        lua_pushvalue(L, i);

        mp_encode_lua_type(L,buf,0);

        lua_pushlstring(L,(char*)buf->b,buf->len);

        /* Reuse the buffer for the next operation by
         * setting its free count to the total buffer size
         * and the current position to zero. */
        buf->free += buf->len;
        buf->len = 0;
    }
    mp_buf_free(L, buf);

    /* Concatenate all nargs buffers together */
    lua_concat(L, nargs);
    return 1;
}

/* ------------------------------- Decoding --------------------------------- */

void mp_decode_to_lua_type(lua_State *L, mp_cur *c);

void mp_decode_to_lua_array(lua_State *L, mp_cur *c, size_t len) {
    assert(len <= UINT_MAX);
    int index = 1;

    lua_newtable(L);
    luaL_checkstack(L, 1, "in function mp_decode_to_lua_array");
    while(len--) {
        lua_pushnumber(L,index++);
        mp_decode_to_lua_type(L,c);
        if (c->err) return;
        lua_settable(L,-3);
    }
}

void mp_decode_to_lua_hash(lua_State *L, mp_cur *c, size_t len) {
    assert(len <= UINT_MAX);
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

    /* If we return more than 18 elements, we must resize the stack to
     * fit all our return values.  But, there is no way to
     * determine how many objects a msgpack will unpack to up front, so
     * we request a +1 larger stack on each iteration (noop if stack is
     * big enough, and when stack does require resize it doubles in size) */
    luaL_checkstack(L, 1,
        "too many return values at once; "
        "use unpack_one or unpack_limit instead.");

    switch(c->p[0]) {
    case 0xcc:  /* uint 8 */
        mp_cur_need(c,2);
        lua_pushunsigned(L,c->p[1]);
        mp_cur_consume(c,2);
        break;
    case 0xd0:  /* int 8 */
        mp_cur_need(c,2);
        lua_pushinteger(L,(signed char)c->p[1]);
        mp_cur_consume(c,2);
        break;
    case 0xcd:  /* uint 16 */
        mp_cur_need(c,3);
        lua_pushunsigned(L,
            (c->p[1] << 8) |
             c->p[2]);
        mp_cur_consume(c,3);
        break;
    case 0xd1:  /* int 16 */
        mp_cur_need(c,3);
        lua_pushinteger(L,(int16_t)
            (c->p[1] << 8) |
             c->p[2]);
        mp_cur_consume(c,3);
        break;
    case 0xce:  /* uint 32 */
        mp_cur_need(c,5);
        lua_pushunsigned(L,
            ((uint32_t)c->p[1] << 24) |
            ((uint32_t)c->p[2] << 16) |
            ((uint32_t)c->p[3] << 8) |
             (uint32_t)c->p[4]);
        mp_cur_consume(c,5);
        break;
    case 0xd2:  /* int 32 */
        mp_cur_need(c,5);
        lua_pushinteger(L,
            ((int32_t)c->p[1] << 24) |
            ((int32_t)c->p[2] << 16) |
            ((int32_t)c->p[3] << 8) |
             (int32_t)c->p[4]);
        mp_cur_consume(c,5);
        break;
    case 0xcf:  /* uint 64 */
        mp_cur_need(c,9);
        lua_pushunsigned(L,
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
#if LUA_VERSION_NUM < 503
        lua_pushnumber(L,
#else
        lua_pushinteger(L,
#endif
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
    case 0xd9:  /* raw 8 */
        mp_cur_need(c,2);
        {
            size_t l = c->p[1];
            mp_cur_need(c,2+l);
            lua_pushlstring(L,(char*)c->p+2,l);
            mp_cur_consume(c,2+l);
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
            size_t l = ((size_t)c->p[1] << 24) |
                       ((size_t)c->p[2] << 16) |
                       ((size_t)c->p[3] << 8) |
                       (size_t)c->p[4];
            mp_cur_consume(c,5);
            mp_cur_need(c,l);
            lua_pushlstring(L,(char*)c->p,l);
            mp_cur_consume(c,l);
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
            size_t l = ((size_t)c->p[1] << 24) |
                       ((size_t)c->p[2] << 16) |
                       ((size_t)c->p[3] << 8) |
                       (size_t)c->p[4];
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
            size_t l = ((size_t)c->p[1] << 24) |
                       ((size_t)c->p[2] << 16) |
                       ((size_t)c->p[3] << 8) |
                       (size_t)c->p[4];
            mp_cur_consume(c,5);
            mp_decode_to_lua_hash(L,c,l);
        }
        break;
    default:    /* types that can't be idenitified by first byte value. */
        if ((c->p[0] & 0x80) == 0) {   /* positive fixnum */
            lua_pushunsigned(L,c->p[0]);
            mp_cur_consume(c,1);
        } else if ((c->p[0] & 0xe0) == 0xe0) {  /* negative fixnum */
            lua_pushinteger(L,(signed char)c->p[0]);
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

int mp_unpack_full(lua_State *L, lua_Integer limit, lua_Integer offset) {
    size_t len;
    const char *s;
    mp_cur c;
    int cnt; /* Number of objects unpacked */
    int decode_all = (!limit && !offset);

    s = luaL_checklstring(L,1,&len); /* if no match, exits */

    if (offset < 0 || limit < 0) /* requesting negative off or lim is invalid */
        return luaL_error(L,
            "Invalid request to unpack with offset of %d and limit of %d.",
            (int) offset, (int) len);
    else if (offset > len)
        return luaL_error(L,
            "Start offset %d greater than input length %d.", (int) offset, (int) len);

    if (decode_all) limit = INT_MAX;

    mp_cur_init(&c,(const unsigned char *)s+offset,len-offset);

    /* We loop over the decode because this could be a stream
     * of multiple top-level values serialized together */
    for(cnt = 0; c.left > 0 && cnt < limit; cnt++) {
        mp_decode_to_lua_type(L,&c);

        if (c.err == MP_CUR_ERROR_EOF) {
            return luaL_error(L,"Missing bytes in input.");
        } else if (c.err == MP_CUR_ERROR_BADFMT) {
            return luaL_error(L,"Bad data format in input.");
        }
    }

    if (!decode_all) {
        /* c->left is the remaining size of the input buffer.
         * subtract the entire buffer size from the unprocessed size
         * to get our next start offset */
        size_t new_offset = len - c.left;
        if (new_offset > LONG_MAX) abort();

        luaL_checkstack(L, 1, "in function mp_unpack_full");

        /* Return offset -1 when we have have processed the entire buffer. */
        lua_pushinteger(L, c.left == 0 ? -1 : (lua_Integer) new_offset);
        /* Results are returned with the arg elements still
         * in place. Lua takes care of only returning
         * elements above the args for us.
         * In this case, we have one arg on the stack
         * for this function, so we insert our first return
         * value at position 2. */
        lua_insert(L, 2);
        cnt += 1; /* increase return count by one to make room for offset */
    }

    return cnt;
}

int mp_unpack(lua_State *L) {
    return mp_unpack_full(L, 0, 0);
}

int mp_unpack_one(lua_State *L) {
    lua_Integer offset = luaL_optinteger(L, 2, 0);
    /* Variable pop because offset may not exist */
    lua_pop(L, lua_gettop(L)-1);
    return mp_unpack_full(L, 1, offset);
}

int mp_unpack_limit(lua_State *L) {
    lua_Integer limit = luaL_checkinteger(L, 2);
    lua_Integer offset = luaL_optinteger(L, 3, 0);
    /* Variable pop because offset may not exist */
    lua_pop(L, lua_gettop(L)-1);

    return mp_unpack_full(L, limit, offset);
}

int mp_safe(lua_State *L) {
    int argc, err, total_results;

    argc = lua_gettop(L);

    /* This adds our function to the bottom of the stack
     * (the "call this function" position) */
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);

    err = lua_pcall(L, argc, LUA_MULTRET, 0);
    total_results = lua_gettop(L);

    if (!err) {
        return total_results;
    } else {
        lua_pushnil(L);
        lua_insert(L,-2);
        return 2;
    }
}

/* -------------------------------------------------------------------------- */
const struct luaL_Reg cmds[] = {
    {"pack", mp_pack},
    {"unpack", mp_unpack},
    {"unpack_one", mp_unpack_one},
    {"unpack_limit", mp_unpack_limit},
    {0}
};

int luaopen_create(lua_State *L) {
    int i;
    /* Manually construct our module table instead of
     * relying on _register or _newlib */
    lua_newtable(L);

    for (i = 0; i < (sizeof(cmds)/sizeof(*cmds) - 1); i++) {
        lua_pushcfunction(L, cmds[i].func);
        lua_setfield(L, -2, cmds[i].name);
    }

    /* Add metadata */
    lua_pushliteral(L, LUACMSGPACK_NAME);
    lua_setfield(L, -2, "_NAME");
    lua_pushliteral(L, LUACMSGPACK_VERSION);
    lua_setfield(L, -2, "_VERSION");
    lua_pushliteral(L, LUACMSGPACK_COPYRIGHT);
    lua_setfield(L, -2, "_COPYRIGHT");
    lua_pushliteral(L, LUACMSGPACK_DESCRIPTION);
    lua_setfield(L, -2, "_DESCRIPTION");
    return 1;
}

LUALIB_API int luaopen_cmsgpack(lua_State *L) {
    luaopen_create(L);

#if LUA_VERSION_NUM < 502
    /* Register name globally for 5.1 */
    lua_pushvalue(L, -1);
    lua_setglobal(L, LUACMSGPACK_NAME);
#endif

    return 1;
}

LUALIB_API int luaopen_cmsgpack_safe(lua_State *L) {
    int i;

    luaopen_cmsgpack(L);

    /* Wrap all functions in the safe handler */
    for (i = 0; i < (sizeof(cmds)/sizeof(*cmds) - 1); i++) {
        lua_getfield(L, -1, cmds[i].name);
        lua_pushcclosure(L, mp_safe, 1);
        lua_setfield(L, -2, cmds[i].name);
    }

#if LUA_VERSION_NUM < 502
    /* Register name globally for 5.1 */
    lua_pushvalue(L, -1);
    lua_setglobal(L, LUACMSGPACK_SAFE_NAME);
#endif

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

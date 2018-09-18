/*
 * Providing murmur2 method implementation of the HyperLogLog algorithm to lua.
 */

#include <string.h>
#include <stdint.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lhash.h"

extern uint64_t MurmurHash64A(const void * key, int len, unsigned int seed);

unsigned int TrailingZeros(unsigned int v)
{
    /*
     * c will be the number of zero bits on the right,
     * so if v is 1101000 (base 2), the c will be 3.
     * NOTE: if v == 0, then c= 31.
     */
    unsigned int c;

    if (v & 0x1) {
        c = 0;
    } else {
        c = 1;

        if ((v & 0xFFFF) == 0) {
            v >>= 16;
            c += 16;
        }

        if ((v & 0xFF) == 0) {
            v >>= 8;
            c += 8;
        }

        if ((v & 0xF) == 0) {
            v >>= 4;
            c += 4;
        }

        if ((v & 0x3) == 0) {
            v >>= 2;
            c += 2;
        }

        c -= v & 0x1;
    }

    return c;

}

static int lua_murmurHash64A (lua_State *L)
{
    const char *s = luaL_checkstring(L, 1);
    uint32_t r = MurmurHash64A(s, strlen(s), seed);
    lua_pushinteger(L, r & MASK31);

    return 1;
}

static int lua_trailing_zeros (lua_State *L)
{
    const uint32_t *i = luaL_checkinteger(L, 1);
    uint32_t r = TrailingZeros(i);
    lua_pushinteger(L, r & MASK31);
    return 1;
}

static int lua_set_seed (lua_State *L)
{
    seed = luaL_checkinteger(L, 1);
    return 0;
}

static const luaL_Reg reg[] = {
    { "murmur2", lua_murmurHash64A },
    { "set_seed", lua_set_seed },
    { "trailing_zeros", lua_trailing_zeros },
    { NULL, NULL }
};

LUALIB_API int luaopen_hash (lua_State *l)
{
    luaL_register(l, LUA_HASHLIBNAME, reg);

    return 1;
}


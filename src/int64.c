#include <lua.h>
#include <lauxlib.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#include <stdbool.h>

static int64_t
_int64(lua_State *lua, int index) {
    int type = lua_type(lua, index);
    int64_t n = 0;

    switch(type) {
        case LUA_TNUMBER: {
            lua_Number d = lua_tonumber(lua, index);
            n = (int64_t)d;
            break;
        }
        case LUA_TSTRING: {
            int negative = 0;
            size_t len = 0;
            uint64_t n64 = 0;

            const uint8_t * str = (const uint8_t *)lua_tolstring(lua, index, &len);
            if (len <= 0 || len > 19) {
                return luaL_error(lua, "The string (length = %d) is not an int64 string", len);
            }

            size_t i = 0;
            if (str[i] == '-') {
                negative = 1;
                ++i;
            }

            for (; i < len; i++) {
                char c = str[i];
                if ( c > '9' || c < '0' ) {
                    return luaL_error(lua, "Bad format input: %s", str);
                }
                n64 = (uint64_t)(str[i] - '0') + 10 * n64;
            }

            n = (int64_t)n64;

            if (negative) {
                n = 0 - n;
            }

            break;
        }
        case LUA_TLIGHTUSERDATA: {
            void * p = lua_touserdata(lua, index);
            n = (intptr_t)p;
            break;
        }
        default: {
            return luaL_error(lua, "argument %d error type %s", index, lua_typename(lua, type));
        }
    }
    return n;
}

static inline void
_pushint64(lua_State *lua, int64_t n) {
    void * p = (void *)(intptr_t)n;
    lua_pushlightuserdata(lua, p);
}

static int
int64Add(lua_State *lua) {
    int64_t a = _int64(lua, 1);
    int64_t b = _int64(lua, 2);
    _pushint64(lua, a + b);
    
    return 1;
}

static int
int64Sub(lua_State *lua) {
    int64_t a = _int64(lua, 1);
    int64_t b = _int64(lua, 2);
    _pushint64(lua, a - b);
    
    return 1;
}

static int
int64Mul(lua_State *lua) {
    int64_t a = _int64(lua, 1);
    int64_t b = _int64(lua, 2);
    _pushint64(lua, a * b);
    
    return 1;
}

static int
int64Div(lua_State *lua) {
    int64_t a = _int64(lua, 1);
    int64_t b = _int64(lua, 2);
    if (b == 0) {
        return luaL_error(lua, "div by zero");
    }
    _pushint64(lua, a / b);
    
    return 1;
}

static int
int64Mod(lua_State *lua) {
    int64_t a = _int64(lua, 1);
    int64_t b = _int64(lua, 2);
    if (b == 0) {
        return luaL_error(lua, "mod by zero");
    }
    _pushint64(lua, a % b);
    
    return 1;
}

static int64_t
_pow64(int64_t a, int64_t b) {

    if (b == 1) {
        return a;
    }

    int64_t a2 = a * a;
    if (b % 2 == 1) {
        return _pow64(a2, b / 2) * a;
    } else {
        return _pow64(a2, b / 2);
    }

}

static int
int64Pow(lua_State *lua) {

    int64_t a = _int64(lua, 1);
    int64_t b = _int64(lua, 2);
    int64_t p;

    if (b > 0) {
        p = _pow64(a, b);
    } else if (b == 0) {
        p = 1;
    } else {
        return luaL_error(lua, "pow by nagtive number %d",(int)b);
    } 
    _pushint64(lua, p);

    return 1;
}

static int
int64Unm(lua_State *lua) {
    int64_t a = _int64(lua, 1);
    _pushint64(lua, -a);

    return 1;
}

static int
int64New(lua_State *lua) {
    int top = lua_gettop(lua);
    int64_t n;
    switch(top) {
        case 0 : 
            lua_pushlightuserdata(lua, NULL);
            break;
        case 1 :
            n = _int64(lua, 1);
            _pushint64(lua, n);
            break;
        default: {
            int base = luaL_checkinteger(lua, 2);
            if (base < 2) {
                luaL_error(lua, "base must be >= 2");
            }
            const char * str = luaL_checkstring(lua, 1);
            n = strtoll(str, NULL, base);
            _pushint64(lua, n);
            break;
        }
    }
    return 1;
}

static int
int64Eq(lua_State *lua) {
    int64_t a = _int64(lua, 1);
    int64_t b = _int64(lua, 2);
    lua_pushboolean(lua, a == b);
    return 1;
}

static int
int64Lt(lua_State *lua) {
    int64_t a = _int64(lua, 1);
    int64_t b = _int64(lua, 2);
    lua_pushboolean(lua, a < b);

    return 1;
}

static int
int64Le(lua_State *lua) {
    int64_t a = _int64(lua, 1);
    int64_t b = _int64(lua, 2);
    lua_pushboolean(lua, a <= b);

    return 1;
}

static int
int64Len(lua_State *lua) {
    int64_t a = _int64(lua, 1);
    lua_pushnumber(lua,(lua_Number)a);

    return 1;
}

static int
tostring(lua_State *lua) {
    static char hex[16] = "0123456789ABCDEF";
    uintptr_t n = (uintptr_t)lua_touserdata(lua, 1);

    int base = 0;

    if (lua_gettop(lua) == 1) {
        base = 10;
    } else {
        base = luaL_checkinteger(lua, 2); 
    }

    int shift, mask;

    switch (base) {
        case 10: {
            int64_t dec = (int64_t)n;

            luaL_Buffer b;
            luaL_buffinit(lua , &b);
            if (dec < 0) {
                luaL_addchar(&b, '-');
                dec = -dec;
            }

            int buffer[32];
            int i;
            for (i = 0; i < 32; i++) {
                buffer[i] = dec % 10;
                dec /= 10;
                if (dec == 0)
                    break;
            }

            while (i >= 0) {
                luaL_addchar(&b, hex[buffer[i]]);
                --i;
            }
            luaL_pushresult(&b);
            return 1;
        }
        case 2:
            shift = 1;
            mask = 1;
            break;
        case 8:
            shift = 3;
            mask = 7;
            break;
        case 16:
            shift = 4;
            mask = 0xf;
            break;
        default:
            return luaL_error(lua, "Unsupport base %d", base);
            break;
    }

    int i;
    char buffer[64];
    for (i = 0; i < 64; i += shift) {
        buffer[i/shift] = hex[(n>>(64-shift-i)) & mask];
    }

    lua_pushlstring(lua, buffer, 64 / shift);
    return 1;
}

static void
_make_mt(lua_State *lua) {
    luaL_Reg lib[] = {
        { "__add", int64Add },
        { "__sub", int64Sub },
        { "__mul", int64Mul },
        { "__div", int64Div },
        { "__mod", int64Mod },
        { "__unm", int64Unm },
        { "__pow", int64Pow },
        { "__eq", int64Eq },
        { "__lt", int64Lt },
        { "__le", int64Le },
        { "__len", int64Len },
        { "__tostring", tostring },
        { NULL, NULL },
    };
    luaL_register(lua, "int64", lib);
}

int
luaopen_int64(lua_State *lua) {

    if (sizeof(intptr_t)!=sizeof(int64_t)) {
        return luaL_error(lua, "Only support 64bit architecture");
    }

    lua_pushlightuserdata(lua, NULL);
    _make_mt(lua);
    lua_setmetatable(lua, -2);
    lua_pop(lua, 1);

    lua_newtable(lua);
    lua_pushcfunction(lua, int64New);
    lua_setfield(lua, -2, "new");
    lua_pushcfunction(lua, tostring);
    lua_setfield(lua, -2, "tostring");
    lua_setglobal(lua, "int64"); 

    return 1;
}


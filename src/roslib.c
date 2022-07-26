#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <locale.h>

/*
Enable setlocale to eliminate differences in the results 
of the sort function in different regions. Note that the 
category is restricted to be set to 'collate' only cuz we
only concern about the result of strcoll() function.
*/
static int r_os_setlocale(lua_State *L){
    int argc = lua_gettop(L);
    char *s;

    if (argc != 1) {
        lua_pushstring(L, "wrong number of arguments");
        return lua_error(L);
    }
    const char *l = luaL_optstring(L, 1, NULL);
    lua_pushstring(L, setlocale(LC_COLLATE, l));
    return 1;
}

static const luaL_Reg ros_funcs[] = {
  {"setlocale", r_os_setlocale},
  {NULL, NULL}
};

LUALIB_API int ros (lua_State *L) {
    luaL_register(L, LUA_OSLIBNAME, ros_funcs);
    return 1;
}

